// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package docgen

import (
	"fmt"
	"go.fuchsia.dev/fuchsia/tools/cppdocgen/clangdoc"
	"io"
	"sort"
	"strings"
)

// Interface for sorting the record list by function name.
type recordByName []*clangdoc.RecordInfo

func (f recordByName) Len() int {
	return len(f)
}
func (f recordByName) Swap(i, j int) {
	f[i], f[j] = f[j], f[i]
}
func (f recordByName) Less(i, j int) bool {
	return f[i].Name < f[j].Name
}

// See seenFuncs variable in WriteClass reference for how it works. This function will avoid adding
// duplicates and will update the map as it adds functions.
func writeBaseClassFunctions(index *Index, r *clangdoc.RecordInfo, headingLevel int, seenFuncs map[string]bool, f io.Writer) {
	// Split out the public member functions.
	var funcs []*clangdoc.FunctionInfo
	for _, fn := range r.ChildFunctions {
		if fn.IsPublic() && !r.IsConstructor(fn) && !r.IsDestructor(fn) {
			key := fn.IdentityKey()
			if !seenFuncs[key] {
				seenFuncs[key] = true

				if !commentContains(fn.Description, NoDocTag) {
					funcs = append(funcs, fn)
				}
			}
		}
	}
	if len(funcs) == 0 {
		return
	}
	sort.Sort(functionByName(funcs))

	// Re-lookup this record by USR because the name contains the full context on the real
	// record but not the Base one.
	//
	// This can be null if we're not documenting the base class.
	fullRecord := index.RecordUsrs[r.USR]

	url := recordLink(index, r)
	if len(url) == 0 {
		// When there's no full record, we need to use the Path+Name as the name (there's
		// no full Namespace record) and there is no documentation link.
		fmt.Fprintf(f, "%s Inherited from %s\n\n",
			headingMarkerAtLevel(headingLevel),
			clangdoc.PathNameToFullyQualified(r.Path, r.Name))
	} else {
		fmt.Fprintf(f, "%s Inherited from [%s](%s)\n\n",
			headingMarkerAtLevel(headingLevel), recordFullName(fullRecord), url)
	}

	if true {
		// Write out the functions as declarations.
		writePreHeader(f)
		for _, fn := range funcs {
			writeFunctionDeclaration(fn, "", 0, true, memberFunctionLink(index, r, fn), f)
		}
		writePreFooter(f)
	} else {
		// Write out the functions (as links when possible).
		for _, fn := range funcs {
			url := memberFunctionLink(index, r, fn)
			if len(url) == 0 {
				fmt.Fprintf(f, "  - %s\n\n", fn.Name)
			} else {
				fmt.Fprintf(f, "  - [%s](%s)\n\n", fn.Name, url)
			}
		}
	}
}

func writeRecordReference(settings WriteSettings, index *Index, h *Header, r *clangdoc.RecordInfo, f io.Writer) {
	fullName := recordFullName(r)
	// Devsite uses {:#htmlId} to give the title a custom ID.
	fmt.Fprintf(f, "## %s {:#%s}\n\n",
		titleWithTemplateSpecializations(fullName, r.Template, "", recordKind(r)),
		recordHtmlId(index, r))

	// This prefix is used for function names. Don't include the full scope (like namespaces)
	// because this will be printed as a declaration where namespaces are not used. This will
	// look weird. There should be enough context with just the class name (and template
	// specialization parameters) for it to make sense.
	//
	// This is used for titles (needs non-syntax highlighted version) and code (wants syntax
	// highlighting). The namePrefixCharLen is the length not counting escaping and HTML tags
	// which is used for function parameter alignment.
	namePrefix := ""
	namePrefixWithSyntax := ""
	namePrefixCharLen := 0
	if r.Template != nil && r.Template.Specialization != nil {
		templateParams, templateParamsCharLen := getTemplateParameterList(r.Template.Specialization.Params, false)
		namePrefix = r.Name + templateParams + "::"
		namePrefixCharLen = len(r.Name) + templateParamsCharLen + 2 // Extra 2 for "::".

		// Get syntax-highlighted version (should have the same # visible chars).
		templateParams, templateParamsCharLen = getTemplateParameterList(r.Template.Specialization.Params, true)
		namePrefixWithSyntax = r.Name + templateParams + "::"
	} else {
		namePrefix = r.Name + "::"
		namePrefixCharLen = len(r.Name) + 2 // Extra 2 for "::".
		namePrefixWithSyntax = r.Name + "::"
	}

	fmt.Fprintf(f, "[Declaration source code](%s)\n\n", settings.locationSourceLink(r.DefLocation))

	// Split out the member functions.
	var funcs []*clangdoc.FunctionInfo
	var ctors []*clangdoc.FunctionInfo
	for _, fn := range r.ChildFunctions {
		if !fn.IsPublic() {
			continue
		} else if r.IsConstructor(fn) {
			ctors = append(ctors, fn)
		} else if r.IsDestructor(fn) {
			// Ignore destructors. It's not currently clear how to present this as
			// there's almost never any documentation for these. The main relevant part
			// is whether the destructor might be private in some cases like internally
			// reference counted classes.
		} else if !commentContains(fn.Description, NoDocTag) {
			funcs = append(funcs, fn)
		}
	}

	groupedFuncs := h.groupFunctions(funcs)
	groupedCtors := h.groupFunctions(ctors)

	// Collect the public records to output.
	var dataMembers []clangdoc.MemberTypeInfo
	for _, m := range r.Members {
		if m.IsPublic() {
			dataMembers = append(dataMembers, m)
		}
	}

	if !commentContains(r.Description, NoDeclTag) {
		writeRecordDeclarationBlock(index, r, dataMembers, f)
	}
	writeComment(index, r.Description, markdownHeading2, f)

	// Data member documentation first.
	for _, d := range dataMembers {
		// Only add separate documentation when there is some. Unlike functions, the
		// class/struct code declaration includes all data members and our code often
		// doesn't have documentation for obvious ones.
		if len(d.Description) > 0 && !commentContains(d.Description, NoDocTag) {
			// TODO we can consider adding an anchor ref here if we ever need to
			// be able to link to members (currently there is no need for this).
			fmt.Fprintf(f, "### %s\n\n", d.Name)
			writeComment(index, d.Description, markdownHeading3, f)
		}
	}

	if len(groupedCtors) > 0 {
		fmt.Fprintf(f, "### Constructor{:#%s}\n\n", functionHtmlId(ctors[0]))
		for _, g := range groupedCtors {
			writeFunctionGroupBody(settings, index, g, namePrefix, namePrefixCharLen,
				false, f)
		}
	}

	// Tracks whether we have seen a given function so we don't duplicate it. Functions can
	// appear multiple times, one for each base class they appear in and once for the class'
	// implementation. If an implemented function declaration in base class' base class, it will
	// appear three times, once for each base and once for the implementation.
	//
	// Here, we only want to show it the first time. This is indexed by each function's
	// IdentityKey().
	seenFuncs := make(map[string]bool)

	// This needs to be done in reverse order because the comments are normally associated with the
	// lowest-level base class that declares the function.
	for i := len(r.Bases) - 1; i >= 0; i-- {
		writeBaseClassFunctions(index, r.Bases[i], 3, seenFuncs, f)
	}

	// Member functions.
	for _, g := range groupedFuncs {
		// Don't include functions that were included in the base class list above.
		// If a section of functions includes both ones from the base class and not, include
		// all of them.
		hasNonBaseClass := false
		for _, fn := range g.Funcs {
			if !seenFuncs[fn.IdentityKey()] {
				hasNonBaseClass = true
			}
		}

		if hasNonBaseClass {
			// Don't fully qualify the name since including namespaces looks too noisy.
			// Just include the class name for clarity.
			if len(g.ExplicitTitle) != 0 {
				fmt.Fprintf(f, "### %s {:#%s}\n\n", g.ExplicitTitle, functionGroupHtmlId(g))
			} else {
				fmt.Fprintf(f, "### %s%s%s {:#%s}\n\n",
					namePrefix, g.Funcs[0].Name,
					functionGroupEllipsesParens(g), functionGroupHtmlId(g))
			}

			writeFunctionGroupBody(settings, index, g, namePrefixWithSyntax,
				namePrefixCharLen, true, f)
		}
	}
}

func recordFullName(r *clangdoc.RecordInfo) string {
	return getScopeQualifier(r.Namespace, true) + r.Name
}

// Returns the empty string if the record does not have emitted documentation.
func recordHtmlId(index *Index, r *clangdoc.RecordInfo) string {
	// Use the fully-qualified type name as the ID.
	//
	// TODO(fxbug.dev/119085) Allow links to template specializations (the same name). If this
	// function took an Index parameter, it could check for this case and use the USR id for all
	// but the first instance.
	fullRecord := index.RecordUsrs[r.USR]
	if fullRecord == nil {
		return ""
	}
	return recordFullName(fullRecord)
}

// Returns the empty string if the record does not have emitted documentation.
func recordLink(index *Index, r *clangdoc.RecordInfo) string {
	fullRecord := index.RecordUsrs[r.USR]
	if fullRecord == nil {
		return ""
	}
	return HeaderReferenceFile(fullRecord.DefLocation.Filename) + "#" + recordHtmlId(index, r)
}

// Returns the empty string if the record isn't documented.
func memberFunctionLink(index *Index, r *clangdoc.RecordInfo, f *clangdoc.FunctionInfo) string {
	fullRecord := index.RecordUsrs[r.USR]
	if fullRecord == nil {
		return ""
	}
	return HeaderReferenceFile(fullRecord.DefLocation.Filename) + "#" + functionHtmlId(f)
}

func recordKind(r *clangdoc.RecordInfo) string {
	// The TagType matches the C++ construct except for capitalization ("Class", "Struct", ...).
	return strings.ToLower(r.TagType)
}

func writeRecordDeclarationBlock(index *Index, r *clangdoc.RecordInfo, data []clangdoc.MemberTypeInfo, f io.Writer) {
	// TODO omit this whole function if there are no namespaces, base classes, or members
	// ("class Foo {...};" is pointless by itself).
	writePreHeader(f)

	// This includes any parent struct/class name but not namespaces.
	nsBegin, nsEnd := getNamespaceDecl(r.Namespace)
	fmt.Fprintf(f, "%s", nsBegin)

	// Template definition.
	if r.Template != nil {
		writeTemplateDeclaration(*r.Template, f)
		fmt.Fprintf(f, "\n")
	}

	kind := recordKind(r)
	scopeQualifier := getScopeQualifier(r.Namespace, false)
	fmt.Fprintf(f, "<span class=\"kwd\">%s</span> %s%s", kind, scopeQualifier, r.Name)

	// Template specializations.
	templateSpecCharLen := 0
	if r.Template != nil && r.Template.Specialization != nil {
		templateParams, templateParamsCharLen := getTemplateParameterList(
			r.Template.Specialization.Params, true)
		fmt.Fprintf(f, "%s", templateParams)
		templateSpecCharLen = templateParamsCharLen
	}

	// Rendered character width of the class declaration. This is used to horizontally align the
	// derived class references below.
	declCharLen := len(kind) + 1 + len(scopeQualifier) + len(r.Name) + templateSpecCharLen

	// Base and virtual base class records. The direct base classes are in Parents and
	// VirtualParents but this does not have the access record on it (public/private/protected).
	// The Bases list is in the correct order so use that as the primary key, and look up those
	// in [Virtual]Parents to see if they're direct parents.
	parents := make(map[string]bool) // Set of USR ids of direct parents.
	for _, p := range r.Parents {
		parents[p.USR] = true
	}
	for _, p := range r.VirtualParents {
		parents[p.USR] = true
	}

	emittedBase := false
	for _, b := range r.Bases {
		if !parents[b.USR] {
			continue // Not a direct parent.
		}

		const derivedSeparator = " : "
		if !emittedBase {
			fmt.Fprintf(f, derivedSeparator)
			emittedBase = true
		} else {
			fmt.Fprintf(f, ",\n%s", makeIndent(declCharLen+len(derivedSeparator)))
		}

		if len(b.Access) > 0 {
			fmt.Fprintf(f, "<span class=\"kwd\">%s</span>", strings.ToLower(b.Access))
		}
		if b.IsVirtual {
			fmt.Fprintf(f, "<span class=\"kwd\">virtual</span>")
		}

		baseName, _ := getEscapedTypeName(clangdoc.PathNameToFullyQualified(b.Path, b.Name))

		url := recordLink(index, b)
		if len(url) == 0 {
			fmt.Fprintf(f, " <span class=\"typ\">%s</span>", baseName)
		} else {
			fmt.Fprintf(f, " <span class=\"typ\"><a href=\"%s\">%s</a></span>", url, baseName)
		}
	}

	fmt.Fprintf(f, " {")
	if len(data) == 0 {
		// No members, just output "Foo { ... }" since the functions will be documented
		// later and we don't want to imply the class is empty.
		fmt.Fprintf(f, " <span class=\"com\">...</span> ")
	} else {
		// Output data members.
		fmt.Fprintf(f, "\n")

		// For classes, try to make clear that this only lists the public data.
		if r.TagType == "Class" {
			fmt.Fprintf(f, "  <span class=\"kwd\">public</span>:\n")
			fmt.Fprintf(f, "    <span class=\"com\">// Public data members:</span>\n")
		}

		for _, d := range data {
			if !commentContains(d.Description, NoDocTag) && !commentContains(d.Description, NoDeclTag) {
				tn, _ := getEscapedTypeName(d.TypeRef.QualName)
				fmt.Fprintf(f, "    <span class=\"typ\">%s</span> %s;\n", tn, d.Name)
			}
		}
	}

	fmt.Fprintf(f, "};\n")
	fmt.Fprintf(f, "%s", nsEnd)

	writePreFooter(f)
}
