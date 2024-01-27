// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Testing utilities for a `DiagnosticsHierarchy`.
//!
//! Pretty much the useful [`assert_data_tree`][assert_data_tree] macro plus some utilities for it.

use {
    crate::{
        ArrayContent, ArrayFormat, Bucket, DiagnosticsHierarchy, ExponentialHistogramParams,
        LinearHistogramParams, Property, EXPONENTIAL_HISTOGRAM_EXTRA_SLOTS,
        LINEAR_HISTOGRAM_EXTRA_SLOTS,
    },
    anyhow::{bail, format_err, Error},
    difference::{Changeset, Difference},
    num_traits::One,
    std::{
        borrow::Cow,
        collections::BTreeSet,
        fmt::{Debug, Display, Formatter, Result as FmtResult},
        ops::{Add, AddAssign, MulAssign},
    },
};

/// Macro to simplify creating `TreeAssertion`s. Commonly used indirectly through the second
/// parameter of `assert_data_tree!`. See `assert_data_tree!` for more usage examples.
///
/// Each leaf value must be a type that implements either `PropertyAssertion` or `TreeAssertion`.
///
/// Example:
/// ```
/// // Manual creation of `TreeAssertion`.
/// let mut root = TreeAssertion::new("root", true);
/// root.add_property_assertion("a-string-property", Box::new("expected-string-value"));
/// let mut child = TreeAssertion::new("child", true);
/// child.add_property_assertion("any-property", Box::new(AnyProperty));
/// root.add_child_assertion(child);
/// root.add_child_assertion(opaque_child);
///
/// // Creation with `tree_assertion!`.
/// let root = tree_assertion!(
///     root: {
///         "a-string-property": "expected-string-value",
///         child: {
///             "any-property": AnyProperty,
///         },
///         opaque_child, // required trailing comma for `TreeAssertion` expressions
///     }
/// );
/// ```
///
/// Note that `TreeAssertion`s given directly to the macro must always be followed by `,`.
#[macro_export]
macro_rules! tree_assertion {
    (@build $tree_assertion:expr,) => {};

    // Exact match of tree
    (@build $tree_assertion:expr, var $key:ident: { $($sub:tt)* }) => {{
        #[allow(unused_mut)]
        let mut child_tree_assertion = TreeAssertion::new($key, true);
        $crate::tree_assertion!(@build child_tree_assertion, $($sub)*);
        $tree_assertion.add_child_assertion(child_tree_assertion);
    }};
    (@build $tree_assertion:expr, var $key:ident: { $($sub:tt)* }, $($rest:tt)*) => {{
        $crate::tree_assertion!(@build $tree_assertion, var $key: { $($sub)* });
        $crate::tree_assertion!(@build $tree_assertion, $($rest)*);
    }};

    // Partial match of tree
    (@build $tree_assertion:expr, var $key:ident: contains { $($sub:tt)* }) => {{
        #[allow(unused_mut)]
        let mut child_tree_assertion = TreeAssertion::new($key, false);
        $crate::tree_assertion!(@build child_tree_assertion, $($sub)*);
        $tree_assertion.add_child_assertion(child_tree_assertion);
    }};
    (@build $tree_assertion:expr, var $key:ident: contains { $($sub:tt)* }, $($rest:tt)*) => {{
        $crate::tree_assertion!(@build $tree_assertion, var $key: contains { $($sub)* });
        $crate::tree_assertion!(@build $tree_assertion, $($rest)*);
    }};

    // Matching properties of a tree
    (@build $tree_assertion:expr, var $key:ident: $assertion:expr) => {{
        $tree_assertion.add_property_assertion($key, Box::new($assertion))
    }};
    (@build $tree_assertion:expr, var $key:ident: $assertion:expr, $($rest:tt)*) => {{
        $crate::tree_assertion!(@build $tree_assertion, var $key: $assertion);
        $crate::tree_assertion!(@build $tree_assertion, $($rest)*);
    }};

    // Key identifier format
    (@build $tree_assertion:expr, $key:ident: $($rest:tt)+) => {{
        let key = stringify!($key);
        $crate::tree_assertion!(@build $tree_assertion, var key: $($rest)+);
    }};
    // Allows string literal for key
    (@build $tree_assertion:expr, $key:tt: $($rest:tt)+) => {{
        let key: &'static str = $key;
        $crate::tree_assertion!(@build $tree_assertion, var key: $($rest)+);
    }};
    // Allows an expression that resolves into a String for key
    (@build $tree_assertion:expr, $key:expr => $($rest:tt)+) => {{
        let key_string : String = $key;
        let key = &key_string;
        $crate::tree_assertion!(@build $tree_assertion, var key: $($rest)+);
    }};
    // Allows an expression that resolves into a TreeAssertion
    (@build $tree_assertion:expr, $child_assertion:expr, $($rest:tt)*) => {{
        $tree_assertion.add_child_assertion($child_assertion);
        $crate::tree_assertion!(@build $tree_assertion, $($rest)*);
    }};

    // Entry points
    (var $key:ident: { $($sub:tt)* }) => {{
        use $crate::testing::TreeAssertion;
        #[allow(unused_mut)]
        let mut tree_assertion = TreeAssertion::new($key, true);
        $crate::tree_assertion!(@build tree_assertion, $($sub)*);
        tree_assertion
    }};
    (var $key:ident: contains { $($sub:tt)* }) => {{
        use $crate::testing::TreeAssertion;
        #[allow(unused_mut)]
        let mut tree_assertion = TreeAssertion::new($key, false);
        $crate::tree_assertion!(@build tree_assertion, $($sub)*);
        tree_assertion
    }};
    ($key:ident: $($rest:tt)+) => {{
        let key = stringify!($key);
        $crate::tree_assertion!(var key: $($rest)+)
    }};
    ($key:tt: $($rest:tt)+) => {{
        let key: &'static str = $key;
        $crate::tree_assertion!(var key: $($rest)+)
    }};
}

/// Macro to simplify tree matching in tests. The first argument is the actual tree passed as a
/// `DiagnosticsHierarchyGetter` (e.g. a `DiagnosticsHierarchy` or an `Inspector`). The second argument is given
/// to `tree_assertion!` which creates a `TreeAssertion` to validate the tree.
///
/// Each leaf value must be a type that implements either `PropertyAssertion` or `TreeAssertion`.
///
/// Example:
/// ```
/// // Actual tree
/// let diagnostics_hierarchy = DiagnosticsHierarchy {
///     name: "key".to_string(),
///     properties: vec![
///         Property::String("sub".to_string(), "sub_value".to_string()),
///         Property::String("sub2".to_string(), "sub2_value".to_string()),
///     ],
///     children: vec![
///        DiagnosticsHierarchy {
///            name: "child1".to_string(),
///            properties: vec![
///                Property::Int("child1_sub".to_string(), 10i64),
///            ],
///            children: vec![],
///        },
///        DiagnosticsHierarchy {
///            name: "child2".to_string(),
///            properties: vec![
///                Property::Uint("child2_sub".to_string(), 20u64),
///            ],
///            children: vec![],
///        },
///    ],
/// };
///
/// assert_data_tree!(
///     diagnostics_hierarchy,
///     key: {
///         sub: AnyProperty,   // only verify that `sub` is a property of `key`
///         sub2: "sub2_value",
///         child1: {
///             child1_sub: 10i64,
///         },
///         child2: {
///             child2_sub: 20u64,
///         },
///     }
/// );
/// ```
///
/// In order to do a partial match on a tree, use the `contains` keyword:
/// ```
/// assert_data_tree!(diagnostics_hierarchy, key: contains {
///     sub: "sub_value",
///     child1: contains {},
/// });
/// ```
///
/// In order to do a match on a tree where the keys need to be computed (they are some
/// expression), you'll need to use `=>` instead of `:`:
///
/// ```
/// assert_data_tree!(diagnostics_hierarchy, key: {
///     key_fn() => "value",
/// })
/// ```
/// Note that `key_fn` has to return a `String`.
///
/// The first argument can be an `Inspector`, in which case the whole tree is read from the
/// `Inspector` and matched against:
/// ```
/// let inspector = Inspector::default();
/// assert_data_tree!(inspector, root: {});
/// ```
///
/// `TreeAssertion`s made elsewhere can be included bodily in the macro, but must always be followed
/// by a trailing comma:
/// assert_data_tree!(
///     diagnostics_hierarchy,
///     key: {
///         make_child_tree_assertion(), // required trailing comma
///     }
/// );
///
/// A tree may contain multiple properties or children with the same name. This macro does *not*
/// support matching against them, and will throw an error if it detects duplicates. This is
/// to provide warning for users who accidentally log the same name multiple times, as the
/// behavior for reading properties or children with duplicate names is not well defined.
#[macro_export]
macro_rules! assert_data_tree {
    ($diagnostics_hierarchy:expr, $($rest:tt)+) => {{
        let tree_assertion = $crate::tree_assertion!($($rest)+);

        use $crate::testing::DiagnosticsHierarchyGetter as _;
        if let Err(e) = tree_assertion.run($diagnostics_hierarchy.get_diagnostics_hierarchy().as_ref()) {
            panic!("tree assertion fails: {}", e);
        }
    }};
}

/// Macro to check a hierarchy with a nice JSON diff.
/// The syntax of the `expected` value is the same as that of `hierarchy!`, and
/// essentially the same as `assert_data_tree!`, except that partial tree matching
/// is not supported (i.e. the keyword `contains`).
#[macro_export]
macro_rules! assert_json_diff {
    ($diagnostics_hierarchy:expr, $($rest:tt)+) => {{
        use $crate::testing::JsonGetter as _;

        let expected = $diagnostics_hierarchy.get_pretty_json();
        let actual_hierarchy: DiagnosticsHierarchy = $crate::hierarchy!{$($rest)+};
        let actual = actual_hierarchy.get_pretty_json();

        if actual != expected {
            panic!("{}", $crate::testing::diff_json(&expected, &actual));
        }
    }}
}

/// A type which can function as a "view" into a diagnostics hierarchy, optionally allocating a new
/// instance to service a request.
pub trait DiagnosticsHierarchyGetter<K: Clone> {
    fn get_diagnostics_hierarchy(&self) -> Cow<'_, DiagnosticsHierarchy<K>>;
}

pub fn diff_json(expected: &str, actual: &str) -> Changeset {
    Changeset::new(expected, actual, "")
}

pub trait JsonGetter<K: Clone + AsRef<str>>: DiagnosticsHierarchyGetter<K> {
    fn get_pretty_json(&self) -> String {
        let mut tree = self.get_diagnostics_hierarchy();
        tree.to_mut().sort();
        serde_json::to_string_pretty(&tree).expect("pretty json string")
    }

    fn get_json(&self) -> String {
        let mut tree = self.get_diagnostics_hierarchy();
        tree.to_mut().sort();
        serde_json::to_string(&tree).expect("pretty json string")
    }
}

impl<K: Clone> DiagnosticsHierarchyGetter<K> for DiagnosticsHierarchy<K> {
    fn get_diagnostics_hierarchy(&self) -> Cow<'_, DiagnosticsHierarchy<K>> {
        Cow::Borrowed(self)
    }
}

impl<K: Clone + AsRef<str>, T: DiagnosticsHierarchyGetter<K>> JsonGetter<K> for T {}

/// A difference between expected and actual output.
struct Diff(Changeset);

impl Diff {
    fn new(expected: &dyn Debug, actual: &dyn Debug) -> Self {
        let expected = format!("{:#?}", expected);
        let actual = format!("{:#?}", actual);
        Diff(Changeset::new(&expected, &actual, "\n"))
    }
}

impl Display for Diff {
    fn fmt(&self, f: &mut Formatter<'_>) -> FmtResult {
        writeln!(f, "(-) Expected vs. (+) Actual:")?;
        for diff in &self.0.diffs {
            let (prefix, contents) = match diff {
                Difference::Same(same) => ("  ", same),
                Difference::Add(added) => ("+ ", added),
                Difference::Rem(removed) => ("- ", removed),
            };
            for line in contents.split("\n") {
                writeln!(f, "{}{}", prefix, line)?;
            }
        }
        Ok(())
    }
}

macro_rules! eq_or_bail {
    ($expected:expr, $actual:expr) => {{
        if $expected != $actual {
            let changes = Diff::new(&$expected, &$actual);
            return Err(format_err!("\n {}", changes));
        }
    }};
    ($expected:expr, $actual:expr, $($args:tt)+) => {{
        if $expected != $actual {
            let changes = Diff::new(&$expected, &$actual);
            return Err(format_err!("{}:\n {}", format!($($args)+), changes));
        }
    }}
}

/// Struct for matching against a Data tree (DiagnosticsHierarchy).
pub struct TreeAssertion<K = String> {
    /// Expected name of the node being compared against
    name: String,
    /// Friendly name that includes path from ancestors. Mainly used to indicate which node fails
    /// in error message
    path: String,
    /// Expected property names and assertions to match the actual properties against
    properties: Vec<(String, Box<dyn PropertyAssertion<K>>)>,
    /// Assertions to match against child trees
    children: Vec<TreeAssertion<K>>,
    /// Whether all properties and children of the tree should be checked
    exact_match: bool,
}

impl<K> TreeAssertion<K>
where
    K: AsRef<str>,
{
    /// Create a new `TreeAssertion`. The |name| argument is the expected name of the tree to be
    /// compared against. Set |exact_match| to true to specify that all properties and children of
    /// the tree should be checked. To perform partial matching of the tree, set it to false.
    pub fn new(name: &str, exact_match: bool) -> Self {
        Self {
            name: name.to_string(),
            path: name.to_string(),
            properties: vec![],
            children: vec![],
            exact_match,
        }
    }

    /// Adds a property assertion to this tree assertion.
    pub fn add_property_assertion(&mut self, key: &str, assertion: Box<dyn PropertyAssertion<K>>) {
        self.properties.push((key.to_owned(), assertion));
    }

    /// Adds a tree assertion as a child of this tree assertion.
    pub fn add_child_assertion(&mut self, mut assertion: TreeAssertion<K>) {
        assertion.path = format!("{}.{}", self.path, assertion.name);
        self.children.push(assertion);
    }

    /// Check whether |actual| tree satisfies criteria defined by `TreeAssertion`. Return `Ok` if
    /// assertion passes and `Error` if assertion fails.
    pub fn run(&self, actual: &DiagnosticsHierarchy<K>) -> Result<(), Error> {
        eq_or_bail!(self.name, actual.name, "node `{}` - expected node name != actual", self.path);

        if self.exact_match {
            let properties_names = self.properties.iter().map(|p| p.0.to_string());
            let children_names = self.children.iter().map(|c| c.name.to_string());
            let keys: BTreeSet<String> = properties_names.chain(children_names).collect();

            let actual_props = actual.properties.iter().map(|p| p.name().to_string());
            let actual_children = actual.children.iter().map(|c| c.name.to_string());
            let actual_keys: BTreeSet<String> = actual_props.chain(actual_children).collect();
            eq_or_bail!(keys, actual_keys, "node `{}` - expected keys != actual", self.path);
        }

        for (name, assertion) in self.properties.iter() {
            let mut matched = actual.properties.iter().filter(|p| p.key().as_ref() == name);
            let first_match = matched.next();
            if let Some(_second_match) = matched.next() {
                bail!("node `{}` - multiple properties found with name `{}`", self.path, name);
            }
            match first_match {
                Some(property) => {
                    if let Err(e) = assertion.run(property) {
                        bail!(
                            "node `{}` - assertion fails for property `{}`. Reason: {}",
                            self.path,
                            name,
                            e
                        );
                    }
                }
                None => bail!("node `{}` - no property named `{}`", self.path, name),
            }
        }
        for assertion in self.children.iter() {
            let mut matched = actual.children.iter().filter(|c| c.name == assertion.name);
            let first_match = matched.next();
            if let Some(_second_match) = matched.next() {
                bail!(
                    "node `{}` - multiple children found with name `{}`",
                    self.path,
                    assertion.name
                );
            }
            match first_match {
                Some(child) => assertion.run(&child)?,
                None => bail!("node `{}` - no child named `{}`", self.path, assertion.name),
            }
        }
        Ok(())
    }
}

/// Trait implemented by types that can act as properies for assertion.
pub trait PropertyAssertion<K = String> {
    /// Check whether |actual| property satisfies criteria. Return `Ok` if assertion passes and
    /// `Error` if assertion fails.
    fn run(&self, actual: &Property<K>) -> Result<(), Error>;
}

macro_rules! impl_property_assertion {
    ($prop_variant:ident, $($ty:ty),+) => {
        $(
            impl<K> PropertyAssertion<K> for $ty {
                fn run(&self, actual: &Property<K>) -> Result<(), Error> {
                    if let Property::$prop_variant(_key, value, ..) = actual {
                        eq_or_bail!(self, value);
                    } else {
                        return Err(format_err!("expected {}, found {}",
                            stringify!($prop_variant), actual.discriminant_name()));
                    }
                    Ok(())
                }
            }
        )+
    }
}

macro_rules! impl_array_properties_assertion {
    ($prop_variant:ident, $($ty:ty),+) => {
        $(
            /// Asserts primitive arrays
            impl<K> PropertyAssertion<K> for Vec<$ty> {
                fn run(&self, actual: &Property<K>) -> Result<(), Error> {
                    if let Property::$prop_variant(_key, value, ..) = actual {
                        match &value {
                            ArrayContent::Values(values) => eq_or_bail!(self, values),
                            _ => {
                                return Err(format_err!(
                                    "expected a {} array, got a histogram",
                                    stringify!($prop_variant)
                                ));
                            }
                        }
                    } else {
                        return Err(format_err!("expected {}, found {}",
                            stringify!($prop_variant), actual.discriminant_name()));
                    }
                    Ok(())
                }
            }

            /// Asserts an array of buckets
            impl<K> PropertyAssertion<K> for Vec<Bucket<$ty>> {
                fn run(&self, actual: &Property<K>) -> Result<(), Error> {
                    if let Property::$prop_variant(_key, value, ..) = actual {
                        match &value {
                            ArrayContent::Buckets(buckets) => eq_or_bail!(self, buckets),
                            _ => {
                                return Err(format_err!(
                                    "expected a {} array, got a histogram",
                                    stringify!($prop_variant)
                                ));
                            }
                        }
                    } else {
                        return Err(format_err!("expected {}, found {}",
                            stringify!($prop_variant), actual.discriminant_name()));
                    }
                    Ok(())
                }
            }

            /// Asserts a histogram.
            impl<K> PropertyAssertion<K> for HistogramAssertion<$ty> {
                fn run(&self, actual: &Property<K>) -> Result<(), Error> {
                    if let Property::$prop_variant(_key, value, ..) = actual {
                        let expected_content =
                            ArrayContent::new(self.values.clone(), self.format.clone()).map_err(
                                |e| {
                                    format_err!(
                                        "failed to load array content for expected assertion {}: {:?}",
                                        stringify!($prop_variant),
                                        e
                                    )
                                },
                            )?;
                        eq_or_bail!(&expected_content, value);
                    } else {
                        return Err(format_err!(
                            "expected {}, found {}",
                            stringify!($prop_variant),
                            actual.discriminant_name(),
                        ));
                    }
                    Ok(())
                }
            }
        )+
    }
}

impl_property_assertion!(String, &str, String);
impl_property_assertion!(Bytes, Vec<u8>);
impl_property_assertion!(Uint, u64);
impl_property_assertion!(Int, i64);
impl_property_assertion!(Double, f64);
impl_property_assertion!(Bool, bool);
impl_array_properties_assertion!(DoubleArray, f64);
impl_array_properties_assertion!(IntArray, i64);
impl_array_properties_assertion!(UintArray, u64);

/// A PropertyAssertion that always passes
pub struct AnyProperty;

impl<K> PropertyAssertion<K> for AnyProperty {
    fn run(&self, _actual: &Property<K>) -> Result<(), Error> {
        Ok(())
    }
}

/// A PropertyAssertion that passes for non-zero, unsigned integers.
///
/// TODO(fxbug.dev/62447): generalize this to use the >= operator.
pub struct NonZeroUintProperty;

impl<K> PropertyAssertion<K> for NonZeroUintProperty {
    fn run(&self, actual: &Property<K>) -> Result<(), Error> {
        match actual {
            Property::Uint(_, v) if *v != 0 => Ok(()),
            Property::Uint(_, v) if *v == 0 => {
                Err(format_err!("expected non-zero integer, found 0"))
            }
            _ => {
                Err(format_err!("expected non-zero integer, found {}", actual.discriminant_name()))
            }
        }
    }
}

impl<K> PropertyAssertion<K> for Vec<String> {
    fn run(&self, actual: &Property<K>) -> Result<(), Error> {
        let this = self.iter().map(|s| s.as_ref()).collect::<Vec<&str>>();
        this.run(actual)
    }
}

impl<K> PropertyAssertion<K> for Vec<&str> {
    fn run(&self, actual: &Property<K>) -> Result<(), Error> {
        match actual {
            Property::StringList(_key, value) => {
                eq_or_bail!(self, value);
                Ok(())
            }
            _ => Err(format_err!("expected StringList, found {}", actual.discriminant_name())),
        }
    }
}

/// An assertion for a histogram property.
pub struct HistogramAssertion<T> {
    format: ArrayFormat,
    values: Vec<T>,
}

impl<T: MulAssign + AddAssign + PartialOrd + Add<Output = T> + Copy + Default + One>
    HistogramAssertion<T>
{
    /// Creates a new histogram assertion for a linear histogram with the given parameters.
    pub fn linear(params: LinearHistogramParams<T>) -> Self {
        let mut values = vec![T::default(); params.buckets + LINEAR_HISTOGRAM_EXTRA_SLOTS];
        values[0] = params.floor;
        values[1] = params.step_size;
        Self { format: ArrayFormat::LinearHistogram, values }
    }

    /// Creates a new histogram assertion for an exponential histogram with the given parameters.
    pub fn exponential(params: ExponentialHistogramParams<T>) -> Self {
        let mut values = vec![T::default(); params.buckets + EXPONENTIAL_HISTOGRAM_EXTRA_SLOTS];
        values[0] = params.floor;
        values[1] = params.initial_step;
        values[2] = params.step_multiplier;
        Self { format: ArrayFormat::ExponentialHistogram, values }
    }

    /// Inserts the list of values to the histogram for asserting them.
    pub fn insert_values(&mut self, values: impl IntoIterator<Item = T>) {
        match self.format {
            ArrayFormat::ExponentialHistogram => {
                for value in values {
                    self.insert_exp(value);
                }
            }
            ArrayFormat::LinearHistogram => {
                for value in values {
                    self.insert_linear(value);
                }
            }
            ArrayFormat::Default => {
                unreachable!("can't construct a histogram assertion for arrays");
            }
        }
    }

    fn insert_linear(&mut self, value: T) {
        let value_index = {
            let mut current_floor = self.values[0];
            let step_size = self.values[1];
            // Start in the underflow index.
            let mut index = LINEAR_HISTOGRAM_EXTRA_SLOTS - 2;
            while value >= current_floor && index < self.values.len() - 1 {
                current_floor += step_size;
                index += 1;
            }
            index as usize
        };
        self.values[value_index] += T::one();
    }

    fn insert_exp(&mut self, value: T) {
        let value_index = {
            let floor = self.values[0];
            let mut current_floor = self.values[0];
            let mut offset = self.values[1];
            let step_multiplier = self.values[2];
            // Start in the underflow index.
            let mut index = EXPONENTIAL_HISTOGRAM_EXTRA_SLOTS - 2;
            while value >= current_floor && index < self.values.len() - 1 {
                current_floor = floor + offset;
                offset *= step_multiplier;
                index += 1;
            }
            index as usize
        };
        self.values[value_index] += T::one();
    }
}

#[cfg(test)]
mod tests {
    use {super::*, crate::Bucket};

    #[fuchsia::test]
    fn test_assert_json_diff() {
        assert_json_diff!(
            simple_tree(),
             key: {
                sub: "sub_value",
                sub2: "sub2_value",
            }
        );

        let diagnostics_hierarchy = complex_tree();
        assert_json_diff!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub2: "sub2_value",
            child1: {
                child1_sub: 10i64,
            },
            child2: {
                child2_sub: 20u64,
            },
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_panicking_assert_json_diff() {
        assert_json_diff!(
            simple_tree(),
             key: {
                sub: "sub_value",
                sb2: "sub2_value",
            }
        );

        let diagnostics_hierarchy = complex_tree();
        assert_json_diff!(diagnostics_hierarchy, key: {
            sb: "sub_value",
            sub2: "sub2_value",
            child: {
                child1_sub: 10i64,
            },
            child3: {
                child2_sub: 20u64,
            },
        });
    }

    #[fuchsia::test]
    fn test_exact_match_simple() {
        let diagnostics_hierarchy = simple_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub2: "sub2_value",
        });
    }

    #[fuchsia::test]
    fn test_exact_match_complex() {
        let diagnostics_hierarchy = complex_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub2: "sub2_value",
            child1: {
                child1_sub: 10i64,
            },
            child2: {
                child2_sub: 20u64,
            },
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_exact_match_mismatched_property_name() {
        let diagnostics_hierarchy = simple_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub3: "sub2_value",
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_exact_match_mismatched_child_name() {
        let diagnostics_hierarchy = complex_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub2: "sub2_value",
            child1: {
                child1_sub: 10i64,
            },
            child3: {
                child2_sub: 20u64,
            },
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_exact_match_mismatched_property_name_in_child() {
        let diagnostics_hierarchy = complex_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub2: "sub2_value",
            child1: {
                child2_sub: 10i64,
            },
            child2: {
                child2_sub: 20u64,
            },
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_exact_match_mismatched_property_value() {
        let diagnostics_hierarchy = simple_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub2_value",
            sub2: "sub2_value",
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_exact_match_missing_property() {
        let diagnostics_hierarchy = simple_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_exact_match_missing_child() {
        let diagnostics_hierarchy = complex_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub2: "sub2_value",
            child1: {
                child1_sub: 10i64,
            },
        });
    }

    #[fuchsia::test]
    fn test_partial_match_success() {
        let diagnostics_hierarchy = complex_tree();

        // only verify the top tree name
        assert_data_tree!(diagnostics_hierarchy, key: contains {});

        // verify parts of the tree
        assert_data_tree!(diagnostics_hierarchy, key: contains {
            sub: "sub_value",
            child1: contains {},
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_partial_match_nonexistent_property() {
        let diagnostics_hierarchy = simple_tree();
        assert_data_tree!(diagnostics_hierarchy, key: contains {
            sub3: AnyProperty,
        });
    }

    #[fuchsia::test]
    fn test_ignore_property_value() {
        let diagnostics_hierarchy = simple_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: AnyProperty,
            sub2: "sub2_value",
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_ignore_property_value_property_name_is_still_checked() {
        let diagnostics_hierarchy = simple_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub1: AnyProperty,
            sub2: "sub2_value",
        })
    }

    #[fuchsia::test]
    fn test_expr_key_syntax() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![Property::String("@time".to_string(), "1.000".to_string())],
            vec![],
        );
        assert_data_tree!(diagnostics_hierarchy, key: {
            "@time": "1.000"
        });
    }

    #[fuchsia::test]
    fn test_var_key_syntax() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![Property::String("@time".to_string(), "1.000".to_string())],
            vec![],
        );
        let time_key = "@time";
        assert_data_tree!(diagnostics_hierarchy, key: {
            var time_key: "1.000"
        });
    }

    #[fuchsia::test]
    fn test_arrays() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::UintArray("@uints".to_string(), ArrayContent::Values(vec![1, 2, 3])),
                Property::IntArray("@ints".to_string(), ArrayContent::Values(vec![-2, -4, 0])),
                Property::DoubleArray(
                    "@doubles".to_string(),
                    ArrayContent::Values(vec![1.3, 2.5, -3.6]),
                ),
            ],
            vec![],
        );
        assert_data_tree!(diagnostics_hierarchy, key: {
            "@uints": vec![1u64, 2, 3],
            "@ints": vec![-2i64, -4, 0],
            "@doubles": vec![1.3, 2.5, -3.6]
        });
    }

    #[fuchsia::test]
    fn test_histograms() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::UintArray(
                    "@linear-uints".to_string(),
                    ArrayContent::new(vec![1, 2, 3, 4, 5], ArrayFormat::LinearHistogram).unwrap(),
                ),
                Property::IntArray(
                    "@linear-ints".to_string(),
                    ArrayContent::new(vec![6, 7, 8, 9, 10], ArrayFormat::LinearHistogram).unwrap(),
                ),
                Property::DoubleArray(
                    "@linear-doubles".to_string(),
                    ArrayContent::new(vec![1.0, 2.0, 4.0, 5.0, 6.0], ArrayFormat::LinearHistogram)
                        .unwrap(),
                ),
                Property::UintArray(
                    "@exp-uints".to_string(),
                    ArrayContent::new(vec![2, 4, 6, 8, 10, 12], ArrayFormat::ExponentialHistogram)
                        .unwrap(),
                ),
                Property::IntArray(
                    "@exp-ints".to_string(),
                    ArrayContent::new(vec![1, 3, 5, 7, 9, 11], ArrayFormat::ExponentialHistogram)
                        .unwrap(),
                ),
                Property::DoubleArray(
                    "@exp-doubles".to_string(),
                    ArrayContent::new(
                        vec![1.0, 2.0, 3.0, 4.0, 5.0, 6.0],
                        ArrayFormat::ExponentialHistogram,
                    )
                    .unwrap(),
                ),
            ],
            vec![],
        );
        let mut linear_assertion = HistogramAssertion::linear(LinearHistogramParams {
            floor: 1u64,
            step_size: 2,
            buckets: 1,
        });
        linear_assertion.insert_values(vec![0, 0, 0, 2, 2, 2, 2, 4, 4, 4, 4, 4]);
        let mut exponential_assertion =
            HistogramAssertion::exponential(ExponentialHistogramParams {
                floor: 1.0,
                initial_step: 2.0,
                step_multiplier: 3.0,
                buckets: 1,
            });
        exponential_assertion.insert_values(vec![
            -3.1, -2.2, -1.3, 0.0, 1.1, 1.2, 2.5, 2.8, 2.0, 3.1, 4.2, 5.3, 6.4, 7.5, 8.6,
        ]);
        assert_data_tree!(diagnostics_hierarchy, key: {
            "@linear-uints": linear_assertion,
            "@linear-ints": vec![
                Bucket { floor: i64::MIN, ceiling: 6, count: 8 },
                Bucket { floor: 6, ceiling: 13, count: 9 },
                Bucket { floor: 13, ceiling: i64::MAX, count: 10 }
            ],
            "@linear-doubles": vec![
                Bucket { floor: f64::MIN, ceiling: 1.0, count: 4.0 },
                Bucket { floor: 1.0, ceiling: 3.0, count: 5.0 },
                Bucket { floor: 3.0, ceiling: f64::MAX, count: 6.0 }
            ],
            "@exp-uints": vec![
                Bucket { floor: 0, ceiling: 2, count: 8 },
                Bucket { floor: 2, ceiling: 6, count: 10 },
                Bucket { floor: 6, ceiling: u64::MAX, count: 12 }
            ],
            "@exp-ints": vec![
                Bucket { floor: i64::MIN, ceiling: 1, count: 7 },
                Bucket { floor: 1, ceiling: 4, count: 9 },
                Bucket { floor: 4, ceiling: i64::MAX, count: 11 }
            ],
            "@exp-doubles": exponential_assertion,
        });
    }

    #[fuchsia::test]
    fn test_matching_tree_assertion_expression() {
        let diagnostics_hierarchy = complex_tree();
        let child1 = tree_assertion!(
            child1: {
                child1_sub: 10i64,
            }
        );
        assert_data_tree!(diagnostics_hierarchy, key: {
            sub: "sub_value",
            sub2: "sub2_value",
            child1,
            tree_assertion!(
                child2: {
                    child2_sub: 20u64,
                }
            ),
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_matching_non_unique_property_fails() {
        let diagnostics_hierarchy = non_unique_prop_tree();
        assert_data_tree!(diagnostics_hierarchy, key: { prop: "prop_value#0" });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_matching_non_unique_property_fails_2() {
        let diagnostics_hierarchy = non_unique_prop_tree();
        assert_data_tree!(diagnostics_hierarchy, key: { prop: "prop_value#1" });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_matching_non_unique_property_fails_3() {
        let diagnostics_hierarchy = non_unique_prop_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            prop: "prop_value#0",
            prop: "prop_value#1",
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_matching_non_unique_child_fails() {
        let diagnostics_hierarchy = non_unique_child_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            child: {
                prop: 10i64
            }
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_matching_non_unique_child_fails_2() {
        let diagnostics_hierarchy = non_unique_child_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            child: {
                prop: 20i64
            }
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_matching_non_unique_child_fails_3() {
        let diagnostics_hierarchy = non_unique_child_tree();
        assert_data_tree!(diagnostics_hierarchy, key: {
            child: {
                prop: 10i64,
            },
            child: {
                prop: 20i64,
            },
        });
    }

    #[fuchsia::test]
    fn test_nonzero_uint_property_passes() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::Uint("value1".to_string(), 10u64),
                Property::Uint("value2".to_string(), 20u64),
            ],
            vec![],
        );
        assert_data_tree!(diagnostics_hierarchy, key: {
            value1: NonZeroUintProperty,
            value2: NonZeroUintProperty,
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_nonzero_uint_property_fails() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::Int("value1".to_string(), 10i64),
                Property::Uint("value2".to_string(), 0u64),
                Property::String("value3".to_string(), "string_value".to_string()),
            ],
            vec![],
        );
        assert_data_tree!(diagnostics_hierarchy, key: {
            value1: NonZeroUintProperty,
            value2: NonZeroUintProperty,
            value3: NonZeroUintProperty,
        });
    }

    #[fuchsia::test]
    fn test_string_list() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::StringList("value1".to_string(), vec!["a".to_string(), "b".to_string()]),
                Property::StringList("value2".to_string(), vec!["c".to_string(), "d".to_string()]),
            ],
            vec![],
        );
        assert_data_tree!(diagnostics_hierarchy, key: {
            value1: vec!["a", "b"],
            value2: vec!["c".to_string(), "d".to_string()],
        });
    }

    #[fuchsia::test]
    #[should_panic]
    fn test_string_list_failure() {
        let diagnostics_hierarchy = DiagnosticsHierarchy::new(
            "key",
            vec![Property::StringList(
                "value1".to_string(),
                vec!["a".to_string(), "b".to_string()],
            )],
            vec![],
        );
        assert_data_tree!(diagnostics_hierarchy, key: {
            value1: vec![1i64, 2],
        });
    }

    fn simple_tree() -> DiagnosticsHierarchy {
        DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::String("sub".to_string(), "sub_value".to_string()),
                Property::String("sub2".to_string(), "sub2_value".to_string()),
            ],
            vec![],
        )
    }

    fn complex_tree() -> DiagnosticsHierarchy {
        DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::String("sub".to_string(), "sub_value".to_string()),
                Property::String("sub2".to_string(), "sub2_value".to_string()),
            ],
            vec![
                DiagnosticsHierarchy::new(
                    "child1",
                    vec![Property::Int("child1_sub".to_string(), 10i64)],
                    vec![],
                ),
                DiagnosticsHierarchy::new(
                    "child2",
                    vec![Property::Uint("child2_sub".to_string(), 20u64)],
                    vec![],
                ),
            ],
        )
    }

    fn non_unique_prop_tree() -> DiagnosticsHierarchy {
        DiagnosticsHierarchy::new(
            "key",
            vec![
                Property::String("prop".to_string(), "prop_value#0".to_string()),
                Property::String("prop".to_string(), "prop_value#1".to_string()),
            ],
            vec![],
        )
    }

    fn non_unique_child_tree() -> DiagnosticsHierarchy {
        DiagnosticsHierarchy::new(
            "key",
            vec![],
            vec![
                DiagnosticsHierarchy::new(
                    "child",
                    vec![Property::Int("prop".to_string(), 10i64)],
                    vec![],
                ),
                DiagnosticsHierarchy::new(
                    "child",
                    vec![Property::Int("prop".to_string(), 20i64)],
                    vec![],
                ),
            ],
        )
    }
}
