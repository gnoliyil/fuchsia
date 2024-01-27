// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context, Result};
use ffx_core::ffx_plugin;
use ffx_package_archive_list_args::ListCommand;
use ffx_package_archive_utils::{read_file_entries, ArchiveEntry, FarArchiveReader, FarListReader};
use ffx_writer::Writer;
use humansize::{file_size_opts, FileSize as _};
use prettytable::{cell, format::TableFormat, row, Row, Table};

#[ffx_plugin()]
pub async fn cmd_list(cmd: ListCommand, mut writer: Writer) -> Result<()> {
    let mut archive_reader = FarArchiveReader::new(&cmd.archive)?;
    list_implementaion(cmd, /*table_format=*/ None, &mut writer, &mut archive_reader)
}

// internal implementation to allow injection of a mock
// archive reader.
fn list_implementaion(
    cmd: ListCommand,
    table_format: Option<TableFormat>,
    writer: &mut Writer,
    reader: &mut dyn FarListReader,
) -> Result<()> {
    let mut entries = read_file_entries(reader)?;

    // Sort the list and print.
    entries.sort();

    if writer.is_machine() {
        writer
            .machine(&entries)
            .context("writing machine representation of archive contents list")?;
    } else {
        print_list_table(&cmd, &entries, table_format, writer)
            .context("printing archive contents table")?;
    }
    Ok(())
}

/// Print the list in a table.
fn print_list_table(
    cmd: &ListCommand,
    entries: &Vec<ArchiveEntry>,
    table_format: Option<TableFormat>,
    writer: &mut Writer,
) -> Result<()> {
    if entries.is_empty() {
        writer.line("")?;
        return Ok(());
    }
    let mut table = Table::new();
    let mut header = row!("NAME");
    if cmd.long_format {
        header.add_cell(cell!("PATH"));
        header.add_cell(cell!("LENGTH"));
    }
    table.set_titles(header);
    if let Some(fmt) = table_format {
        table.set_format(fmt);
    }

    for entry in entries {
        let mut row: Row = row![entry.name];

        if cmd.long_format {
            row.add_cell(cell!(entry.path));
            row.add_cell(cell!(entry
                .length
                .map(|n| n
                    .file_size(file_size_opts::CONVENTIONAL)
                    .unwrap_or_else(|_| format!("{}b", n)))
                .unwrap_or_else(|| "missing from archive".into())));
        }

        table.add_row(row);
    }
    table.print(writer)?;

    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;
    use ffx_package_archive_utils::{test_utils::create_mockreader, MockFarListReader};
    use ffx_writer::Format;
    use std::{collections::HashMap, path::PathBuf};

    #[test]
    fn test_list_empty() -> Result<()> {
        let mut mockreader = MockFarListReader::new();
        mockreader.expect_list_contents().returning(|| Ok(vec![]));
        mockreader.expect_list_meta_contents().returning(|| Ok((vec![], HashMap::new())));

        let cmd = ListCommand { archive: PathBuf::from("some_empty.far"), long_format: false };

        let mut writer = Writer::new_test(None);
        list_implementaion(cmd, None, &mut writer, &mut mockreader)?;

        assert_eq!(writer.test_output()?, "\n".to_string());
        Ok(())
    }

    #[test]
    /// Tests reading the "meta.far" directly vs. when part of a
    /// larger archive.
    fn test_list_with_no_meta() -> Result<()> {
        let mut mockreader = MockFarListReader::new();
        mockreader.expect_list_contents().returning(|| {
            Ok(vec![
                ArchiveEntry {
                    name: "meta/the_component.cm".to_string(),
                    path: "meta/the_component.cm".to_string(),
                    length: Some(100),
                },
                ArchiveEntry {
                    name: "meta/package".to_string(),
                    path: "meta/package".to_string(),
                    length: Some(25),
                },
                ArchiveEntry {
                    name: "meta/contents".to_string(),
                    path: "meta/contents".to_string(),
                    length: Some(55),
                },
            ])
        });
        mockreader.expect_list_meta_contents().returning(|| Ok((vec![], HashMap::new())));

        let cmd = ListCommand { archive: PathBuf::from("just_meta.far"), long_format: false };

        let mut writer = Writer::new_test(None);
        list_implementaion(cmd, None, &mut writer, &mut mockreader)?;

        let expected = r#"
+-----------------------+
| NAME                  |
+=======================+
| meta/contents         |
+-----------------------+
| meta/package          |
+-----------------------+
| meta/the_component.cm |
+-----------------------+
"#[1..]
            .to_string();

        assert_eq!(writer.test_output()?, expected);
        Ok(())
    }

    #[test]
    fn test_list_with_meta() -> Result<()> {
        let cmd = ListCommand { archive: PathBuf::from("just_meta.far"), long_format: false };

        let mut writer = Writer::new_test(None);
        list_implementaion(cmd, None, &mut writer, &mut create_mockreader())?;

        let expected = r#"
+-----------------------+
| NAME                  |
+=======================+
| data/missing_blob     |
+-----------------------+
| data/some_file        |
+-----------------------+
| lib/run.so            |
+-----------------------+
| meta.far              |
+-----------------------+
| meta/contents         |
+-----------------------+
| meta/package          |
+-----------------------+
| meta/the_component.cm |
+-----------------------+
| run_me                |
+-----------------------+
"#[1..]
            .to_string();

        assert_eq!(writer.test_output()?, expected);
        Ok(())
    }

    #[test]
    fn test_list_long_format() -> Result<()> {
        let cmd = ListCommand { archive: PathBuf::from("just_meta.far"), long_format: true };

        let mut writer = Writer::new_test(None);
        list_implementaion(cmd, None, &mut writer, &mut create_mockreader())?;

        let expected = "\
+-----------------------+------------------------------------------------------------------+----------------------+
| NAME                  | PATH                                                             | LENGTH               |
+=======================+==================================================================+======================+
| data/missing_blob     | acfe18f46d86a6d0848ce02320acb455b17f2df9fe5806dc52465b3d74cf2fd9 | missing from archive |
+-----------------------+------------------------------------------------------------------+----------------------+
| data/some_file        | 4ef082296b26108697e851e0b40f8d8d31f96f934d7076f3bad37d5103be172c | 292.97 KB            |
+-----------------------+------------------------------------------------------------------+----------------------+
| lib/run.so            | 892d655f2c841030d1b5556f9f124a753b5e32948471be76e72d330c6b6ba1db | 4 KB                 |
+-----------------------+------------------------------------------------------------------+----------------------+
| meta.far              | meta.far                                                         | 16 KB                |
+-----------------------+------------------------------------------------------------------+----------------------+
| meta/contents         | meta/contents                                                    | 55 B                 |
+-----------------------+------------------------------------------------------------------+----------------------+
| meta/package          | meta/package                                                     | 25 B                 |
+-----------------------+------------------------------------------------------------------+----------------------+
| meta/the_component.cm | meta/the_component.cm                                            | 100 B                |
+-----------------------+------------------------------------------------------------------+----------------------+
| run_me                | 1f487b576253664f9de1a940ad3a350ca47316b5cdb65254fbf267367fd77c62 | 4 KB                 |
+-----------------------+------------------------------------------------------------------+----------------------+
".to_owned();

        assert_eq!(writer.test_output()?, expected);
        Ok(())
    }

    #[test]
    fn test_list_machine() -> Result<()> {
        let cmd = ListCommand { archive: PathBuf::from("just_meta.far"), long_format: false };

        let mut writer = Writer::new_test(Some(Format::JsonPretty));
        list_implementaion(cmd, None, &mut writer, &mut create_mockreader())?;

        let expected = r#"
[
  {
    "name": "data/missing_blob",
    "path": "acfe18f46d86a6d0848ce02320acb455b17f2df9fe5806dc52465b3d74cf2fd9",
    "length": null
  },
  {
    "name": "data/some_file",
    "path": "4ef082296b26108697e851e0b40f8d8d31f96f934d7076f3bad37d5103be172c",
    "length": 300000
  },
  {
    "name": "lib/run.so",
    "path": "892d655f2c841030d1b5556f9f124a753b5e32948471be76e72d330c6b6ba1db",
    "length": 4096
  },
  {
    "name": "meta.far",
    "path": "meta.far",
    "length": 16384
  },
  {
    "name": "meta/contents",
    "path": "meta/contents",
    "length": 55
  },
  {
    "name": "meta/package",
    "path": "meta/package",
    "length": 25
  },
  {
    "name": "meta/the_component.cm",
    "path": "meta/the_component.cm",
    "length": 100
  },
  {
    "name": "run_me",
    "path": "1f487b576253664f9de1a940ad3a350ca47316b5cdb65254fbf267367fd77c62",
    "length": 4096
  }
]"#[1..]
            .to_string();

        assert_eq!(writer.test_output()?, expected);
        Ok(())
    }
}
