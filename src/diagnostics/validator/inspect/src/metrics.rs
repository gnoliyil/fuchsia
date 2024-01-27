// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    fuchsia_inspect::reader::snapshot::ScannedBlock,
    inspect_format::{BlockType, PropertyFormat},
    serde::Serialize,
    std::{self, collections::HashMap},
};

// Blocks such as Node, Extent, and Name may or may not be part of the Inspect tree. We want to
// count each case separately. Also, Extent blocks can't be fully analyzed when first scanned,
// since they don't store their own data size. So there's a three-step process to gather metrics.
// All these take place while Scanner is reading the VMO:
//
// 1) Analyze each block in the VMO with Metrics::analyze().
// 2) While building the tree,
//   2A) set the data size for Extent blocks;
//   2B) record the metrics for all blocks in the tree.
// 3) Record metrics as "NotUsed" for all remaining blocks, first setting their data size to 0.
//
// This can be combined for blocks that are never part of the tree, like Free and Reserved blocks.

// How many bytes are used to store a single number (same for i, u, and f, defined in the VMO spec)
const NUMERIC_TYPE_SIZE: usize = 8;

// Metrics for an individual block - will be remembered alongside the block's data by the Scanner.
#[derive(Debug)]
pub struct BlockMetrics {
    description: String,
    header_bytes: usize,
    data_bytes: usize,
    total_bytes: usize,
}

impl BlockMetrics {
    pub fn set_data_bytes(&mut self, bytes: usize) {
        self.data_bytes = bytes;
    }

    #[cfg(test)]
    pub(crate) fn sample_for_test(
        description: String,
        header_bytes: usize,
        data_bytes: usize,
        total_bytes: usize,
    ) -> BlockMetrics {
        BlockMetrics { description, header_bytes, data_bytes, total_bytes }
    }
}

// Tells whether the block was used in the Inspect tree or not.
#[derive(PartialEq)]
pub(crate) enum BlockStatus {
    Used,
    NotUsed,
}

// Gathers statistics for a type of block.
#[derive(Debug, Serialize, PartialEq)]
pub struct BlockStatistics {
    pub count: u64,
    pub header_bytes: usize,
    pub data_bytes: usize,
    pub total_bytes: usize,
    pub data_percent: u64,
}

impl BlockStatistics {
    fn new() -> BlockStatistics {
        BlockStatistics {
            count: 0,
            header_bytes: 0,
            data_bytes: 0,
            total_bytes: 0,
            data_percent: 0,
        }
    }

    fn update(&mut self, numbers: &BlockMetrics, status: BlockStatus) {
        let BlockMetrics { header_bytes, data_bytes, total_bytes, .. } = numbers;
        self.header_bytes += header_bytes;
        if status == BlockStatus::Used {
            self.data_bytes += data_bytes;
        }
        self.total_bytes += total_bytes;
        if self.total_bytes > 0 {
            self.data_percent = (self.data_bytes * 100 / self.total_bytes) as u64;
        }
    }
}

// Stores statistics for every type (description) of block, plus VMO as a whole.
#[derive(Debug, Serialize)]
pub struct Metrics {
    pub block_count: u64,
    pub size: usize,
    pub block_statistics: HashMap<String, BlockStatistics>,
}

trait Description {
    fn description(&self) -> Result<String, Error>;
}

// Describes the block, distinguishing array and histogram types.
impl Description for ScannedBlock<'_> {
    fn description(&self) -> Result<String, Error> {
        match self.block_type_or()? {
            BlockType::ArrayValue => {
                Ok(format!("ARRAY({:?}, {})", self.array_format()?, self.array_entry_type()?))
            }
            BlockType::BufferValue => match self.property_format()? {
                PropertyFormat::String => Ok("STRING".to_owned()),
                PropertyFormat::Bytes => Ok("BYTES".to_owned()),
            },
            _ => Ok(format!("{}", self.block_type_or()?)),
        }
    }
}

impl Metrics {
    pub fn new() -> Metrics {
        Metrics { block_count: 0, size: 0, block_statistics: HashMap::new() }
    }

    pub(crate) fn record(&mut self, metrics: &BlockMetrics, status: BlockStatus) {
        let description = match status {
            BlockStatus::NotUsed => format!("{}(UNUSED)", metrics.description),
            BlockStatus::Used => metrics.description.clone(),
        };
        let statistics =
            self.block_statistics.entry(description).or_insert_with(|| BlockStatistics::new());
        statistics.count += 1;
        statistics.update(metrics, status);
        self.block_count += 1;
        self.size += metrics.total_bytes;
    }

    // Process (in a single operation) a block of a type that will never be part of the Inspect
    // data tree.
    pub fn process(&mut self, block: ScannedBlock<'_>) -> Result<(), Error> {
        self.record(&Metrics::analyze(block)?, BlockStatus::Used);
        Ok(())
    }

    pub fn analyze(block: ScannedBlock<'_>) -> Result<BlockMetrics, Error> {
        let description = block.description()?;
        let block_type = block.block_type_or()?;

        let data_bytes = match block_type {
            BlockType::Header => 4,
            BlockType::Reserved
            | BlockType::NodeValue
            | BlockType::Free
            | BlockType::BufferValue
            | BlockType::Tombstone
            | BlockType::LinkValue => 0,
            BlockType::IntValue
            | BlockType::UintValue
            | BlockType::DoubleValue
            | BlockType::BoolValue => NUMERIC_TYPE_SIZE,

            BlockType::ArrayValue => NUMERIC_TYPE_SIZE * block.array_slots()?,
            BlockType::Name => block.name_length()?,
            BlockType::Extent => block.extent_contents()?.len(),
            BlockType::StringReference => block.total_length()?,
        };

        let header_bytes = match block_type {
            BlockType::Header
            | BlockType::NodeValue
            | BlockType::BufferValue
            | BlockType::Free
            | BlockType::Reserved
            | BlockType::Tombstone
            | BlockType::ArrayValue
            | BlockType::LinkValue => 16,
            BlockType::StringReference => 12,
            BlockType::IntValue
            | BlockType::DoubleValue
            | BlockType::UintValue
            | BlockType::BoolValue
            | BlockType::Name
            | BlockType::Extent => 8,
        };

        let total_bytes = 16 << block.order();
        Ok(BlockMetrics { description, data_bytes, header_bytes, total_bytes })
    }
}

#[cfg(test)]
mod tests {
    use std::convert::TryFrom;
    use {
        super::*,
        crate::{data, puppet, results::Results},
        anyhow::{bail, format_err},
        inspect_format::{
            constants, ArrayFormat, Block, BlockIndex, BlockType, HeaderFields, PayloadFields,
            PropertyFormat,
        },
    };

    #[fuchsia::test]
    async fn metrics_work() -> Result<(), Error> {
        let puppet = puppet::tests::local_incomplete_puppet().await?;
        let metrics = puppet.metrics().unwrap();
        let mut results = Results::new();
        results.remember_metrics(metrics, "trialfoo", 42, "stepfoo");
        let json = results.to_json();
        assert!(json.contains("\"trial_name\":\"trialfoo\""), "{}", json);
        assert!(json.contains(&format!("\"size\":{}", puppet::VMO_SIZE)), "{}", json);
        assert!(json.contains("\"step_index\":42"), "{}", json);
        assert!(json.contains("\"step_name\":\"stepfoo\""), "{}", json);
        assert!(json.contains("\"block_count\":8"), "{}", json);
        assert!(json.contains("\"HEADER\":{\"count\":1,\"header_bytes\":16,\"data_bytes\":4,\"total_bytes\":32,\"data_percent\":12}"), "{}", json);
        Ok(())
    }

    fn test_metrics(
        buffer: &[u8],
        block_count: u64,
        size: usize,
        description: &str,
        count: u64,
        header_bytes: usize,
        data_bytes: usize,
        total_bytes: usize,
        data_percent: u64,
    ) -> Result<(), Error> {
        let metrics = data::Scanner::try_from(buffer).map(|d| d.metrics())?;
        assert_eq!(metrics.block_count, block_count, "Bad block_count for {}", description);
        assert_eq!(metrics.size, size, "Bad size for {}", description);
        let correct_statistics =
            BlockStatistics { count, header_bytes, data_bytes, total_bytes, data_percent };
        match metrics.block_statistics.get(description) {
            None => {
                return Err(format_err!(
                    "block {} not found in {:?}",
                    description,
                    metrics.block_statistics.keys()
                ))
            }
            Some(statistics) if statistics == &correct_statistics => {}
            Some(unexpected) => bail!(
                "Value mismatch, {:?} vs {:?} for {}",
                unexpected,
                correct_statistics,
                description
            ),
        }
        Ok(())
    }

    fn copy_into(source: &[u8], dest: &mut [u8], index: usize, offset: usize) {
        let offset = index * 16 + offset;
        dest[offset..offset + source.len()].copy_from_slice(source);
    }

    macro_rules! put_header {
        ($block:ident, $index:expr, $buffer:expr) => {
            copy_into(&HeaderFields::value(&$block).to_le_bytes(), $buffer, $index, 0);
        };
    }
    macro_rules! put_payload {
        ($block:ident, $index:expr, $buffer:expr) => {
            copy_into(&PayloadFields::value(&$block).to_le_bytes(), $buffer, $index, 8);
        };
    }
    macro_rules! set_type {
        ($block:ident, $block_type:ident) => {
            HeaderFields::set_block_type(&mut $block, BlockType::$block_type as u8)
        };
    }

    const NAME_INDEX: u32 = 3;

    // Creates the required Header block. Also creates a Name block because
    // lots of things use it.
    // Note that \0 is a valid UTF-8 character so there's no need to set string data.
    fn init_vmo_contents(mut buffer: &mut [u8]) {
        const HEADER_INDEX: usize = 0;

        let mut container = [0u8; 16];
        let mut header = Block::new(&mut container, BlockIndex::EMPTY);
        HeaderFields::set_order(&mut header, constants::HEADER_ORDER as u8);
        set_type!(header, Header);
        HeaderFields::set_header_magic(&mut header, constants::HEADER_MAGIC_NUMBER);
        HeaderFields::set_header_version(&mut header, constants::HEADER_VERSION_NUMBER);
        put_header!(header, HEADER_INDEX, &mut buffer);
        let mut container = [0u8; 16];
        let mut name_header = Block::new(&mut container, BlockIndex::EMPTY);
        set_type!(name_header, Name);
        HeaderFields::set_name_length(&mut name_header, 4);
        put_header!(name_header, NAME_INDEX as usize, &mut buffer);
    }

    #[fuchsia::test]
    fn header_metrics() -> Result<(), Error> {
        let mut buffer = [0u8; 256];
        init_vmo_contents(&mut buffer);
        test_metrics(&buffer, 15, 256, "HEADER", 1, 16, 4, 32, 12)?;
        test_metrics(&buffer, 15, 256, "FREE", 13, 208, 0, 208, 0)?;
        test_metrics(&buffer, 15, 256, "NAME(UNUSED)", 1, 8, 0, 16, 0)?;
        Ok(())
    }

    #[fuchsia::test]
    fn reserved_metrics() -> Result<(), Error> {
        let mut buffer = [0u8; 256];
        init_vmo_contents(&mut buffer);
        let mut container = [0u8; 16];
        let mut reserved_header = Block::new(&mut container, BlockIndex::EMPTY);
        set_type!(reserved_header, Reserved);
        HeaderFields::set_order(&mut reserved_header, 1);
        put_header!(reserved_header, 2, &mut buffer);
        test_metrics(&buffer, 14, 256, "RESERVED", 1, 16, 0, 32, 0)?;
        Ok(())
    }

    #[fuchsia::test]
    fn node_metrics() -> Result<(), Error> {
        let mut buffer = [0u8; 256];
        init_vmo_contents(&mut buffer);
        let mut container = [0u8; 16];
        let mut node_header = Block::new(&mut container, BlockIndex::EMPTY);
        set_type!(node_header, NodeValue);
        HeaderFields::set_value_parent_index(&mut node_header, 1);
        put_header!(node_header, 2, &mut buffer);
        test_metrics(&buffer, 15, 256, "NODE_VALUE(UNUSED)", 1, 16, 0, 16, 0)?;
        HeaderFields::set_value_name_index(&mut node_header, NAME_INDEX);
        HeaderFields::set_value_parent_index(&mut node_header, 0);
        put_header!(node_header, 2, &mut buffer);
        test_metrics(&buffer, 15, 256, "NODE_VALUE", 1, 16, 0, 16, 0)?;
        test_metrics(&buffer, 15, 256, "NAME", 1, 8, 4, 16, 25)?;
        set_type!(node_header, Tombstone);
        put_header!(node_header, 2, &mut buffer);
        test_metrics(&buffer, 15, 256, "TOMBSTONE", 1, 16, 0, 16, 0)?;
        test_metrics(&buffer, 15, 256, "NAME(UNUSED)", 1, 8, 0, 16, 0)?;
        Ok(())
    }

    #[fuchsia::test]
    fn number_metrics() -> Result<(), Error> {
        let mut buffer = [0u8; 256];
        init_vmo_contents(&mut buffer);
        macro_rules! test_number {
            ($number_type:ident, $parent:expr, $block_string:expr, $data_size:expr, $data_percent:expr) => {
                let mut container = [0u8; 16];
                let mut value = Block::new(&mut container, BlockIndex::EMPTY);
                set_type!(value, $number_type);
                HeaderFields::set_value_name_index(&mut value, NAME_INDEX);
                HeaderFields::set_value_parent_index(&mut value, $parent);
                put_header!(value, 2, &mut buffer);
                test_metrics(&buffer, 15, 256, $block_string, 1, 8, $data_size, 16, $data_percent)?;
            };
        }
        test_number!(IntValue, 0, "INT_VALUE", 8, 50);
        test_number!(IntValue, 5, "INT_VALUE(UNUSED)", 0, 0);
        test_number!(DoubleValue, 0, "DOUBLE_VALUE", 8, 50);
        test_number!(DoubleValue, 5, "DOUBLE_VALUE(UNUSED)", 0, 0);
        test_number!(UintValue, 0, "UINT_VALUE", 8, 50);
        test_number!(UintValue, 5, "UINT_VALUE(UNUSED)", 0, 0);
        Ok(())
    }

    #[fuchsia::test]
    fn property_metrics() -> Result<(), Error> {
        let mut buffer = [0u8; 256];
        init_vmo_contents(&mut buffer);
        let mut container = [0u8; 16];
        let mut value = Block::new(&mut container, BlockIndex::EMPTY);
        set_type!(value, BufferValue);
        HeaderFields::set_value_name_index(&mut value, NAME_INDEX);
        HeaderFields::set_value_parent_index(&mut value, 0);
        put_header!(value, 2, &mut buffer);
        PayloadFields::set_property_total_length(&mut value, 12);
        PayloadFields::set_property_extent_index(&mut value, 4);
        PayloadFields::set_property_flags(&mut value, PropertyFormat::String as u8);
        put_payload!(value, 2, &mut buffer);
        let mut container = [0u8; 16];
        let mut extent = Block::new(&mut container, BlockIndex::EMPTY);
        set_type!(extent, Extent);
        HeaderFields::set_extent_next_index(&mut extent, 5);
        put_header!(extent, 4, &mut buffer);
        HeaderFields::set_extent_next_index(&mut extent, 0);
        put_header!(extent, 5, &mut buffer);
        test_metrics(&buffer, 15, 256, "EXTENT", 2, 16, 12, 32, 37)?;
        test_metrics(&buffer, 15, 256, "STRING", 1, 16, 0, 16, 0)?;
        PayloadFields::set_property_flags(&mut value, PropertyFormat::Bytes as u8);
        put_payload!(value, 2, &mut buffer);
        test_metrics(&buffer, 15, 256, "EXTENT", 2, 16, 12, 32, 37)?;
        test_metrics(&buffer, 15, 256, "BYTES", 1, 16, 0, 16, 0)?;
        HeaderFields::set_value_parent_index(&mut value, 7);
        put_header!(value, 2, &mut buffer);
        test_metrics(&buffer, 15, 256, "EXTENT(UNUSED)", 2, 16, 0, 32, 0)?;
        test_metrics(&buffer, 15, 256, "BYTES(UNUSED)", 1, 16, 0, 16, 0)?;
        Ok(())
    }

    #[fuchsia::test]
    fn array_metrics() -> Result<(), Error> {
        let mut buffer = [0u8; 256];
        init_vmo_contents(&mut buffer);
        let mut container = [0u8; 16];
        let mut value = Block::new(&mut container, BlockIndex::EMPTY);
        set_type!(value, ArrayValue);
        HeaderFields::set_order(&mut value, 3);
        HeaderFields::set_value_name_index(&mut value, NAME_INDEX);
        HeaderFields::set_value_parent_index(&mut value, 0);
        put_header!(value, 4, &mut buffer);
        PayloadFields::set_array_entry_type(&mut value, BlockType::IntValue as u8);
        PayloadFields::set_array_flags(&mut value, ArrayFormat::Default as u8);
        PayloadFields::set_array_slots_count(&mut value, 4);
        put_payload!(value, 4, &mut buffer);
        test_metrics(&buffer, 8, 256, "ARRAY(Default, INT_VALUE)", 1, 16, 32, 128, 25)?;
        PayloadFields::set_array_flags(&mut value, ArrayFormat::LinearHistogram as u8);
        PayloadFields::set_array_slots_count(&mut value, 8);
        put_payload!(value, 4, &mut buffer);
        test_metrics(&buffer, 8, 256, "ARRAY(LinearHistogram, INT_VALUE)", 1, 16, 64, 128, 50)?;
        PayloadFields::set_array_flags(&mut value, ArrayFormat::ExponentialHistogram as u8);
        // avoid line-wrapping the parameter list of test_metrics()
        let name = "ARRAY(ExponentialHistogram, INT_VALUE)";
        put_payload!(value, 4, &mut buffer);
        test_metrics(&buffer, 8, 256, name, 1, 16, 64, 128, 50)?;
        PayloadFields::set_array_entry_type(&mut value, BlockType::UintValue as u8);
        let name = "ARRAY(ExponentialHistogram, UINT_VALUE)";
        put_payload!(value, 4, &mut buffer);
        test_metrics(&buffer, 8, 256, name, 1, 16, 64, 128, 50)?;
        PayloadFields::set_array_entry_type(&mut value, BlockType::DoubleValue as u8);
        let name = "ARRAY(ExponentialHistogram, DOUBLE_VALUE)";
        put_payload!(value, 4, &mut buffer);
        test_metrics(&buffer, 8, 256, name, 1, 16, 64, 128, 50)?;
        HeaderFields::set_value_parent_index(&mut value, 1);
        let name = "ARRAY(ExponentialHistogram, DOUBLE_VALUE)(UNUSED)";
        put_header!(value, 4, &mut buffer);
        test_metrics(&buffer, 8, 256, name, 1, 16, 0, 128, 0)?;
        Ok(())
    }
}
