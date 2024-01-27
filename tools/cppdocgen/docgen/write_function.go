// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package docgen

import (
	"fmt"
	"go.fuchsia.dev/fuchsia/tools/cppdocgen/clangdoc"
	"io"
)

// Writes either "()" for functions with no arguments, or "(...)" for functions with arguments.
// This is used to make things look like function calls without listing the arguments or incorrectly
// implying that they take no arguments.
func functionEllipsesParens(fn *clangdoc.FunctionInfo) string {
	if len(fn.Params) == 0 {
		return "()"
	}
	return "(…)"
}
func functionGroupEllipsesParens(g *FunctionGroup) string {
	for _, fn := range g.Funcs {
		if len(fn.Params) > 0 {
			return "(…)"
		}
	}
	return "()"
}

// If linkDest is nonempty this will make the function name a link. The number of visible characters
// in this prefix is supplied, which allows the caller to include HTML tags and escaped characters
// while keeping things aligned.
func writeFunctionDeclaration(fn *clangdoc.FunctionInfo, namePrefix string,
	namePrefixVisibleCharLen int, includeReturnType bool, linkDest string,
	f io.Writer) {
	if fn.Template != nil {
		writeTemplateDeclaration(*fn.Template, f)
		fmt.Fprintf(f, "\n")
	}

	retTypeLen := 0
	if includeReturnType {
		qualType, n := getEscapedTypeName(fn.ReturnType.Reference.QualName)
		fmt.Fprintf(f, "<span class=\"typ\">%s</span> ", qualType)
		retTypeLen = n + 1 // Include space after.
	}

	// Name (optionally linked and with optional prefix).
	if len(linkDest) > 0 {
		fmt.Fprintf(f, "<a href=\"%s\">", linkDest)
	}
	fmt.Fprintf(f, "%s<b>%s</b>", namePrefix, fn.Name)
	if len(linkDest) > 0 {
		fmt.Fprintf(f, "</a>")
	}

	// Template specializations.
	templateSpecCharLen := 0
	if fn.Template != nil && fn.Template.Specialization != nil {
		templateParams := ""
		templateParams, templateSpecCharLen = getTemplateParameterList(
			fn.Template.Specialization.Params, true)
		fmt.Fprintf(f, "%s", templateParams)
	}

	fmt.Fprintf(f, "(")

	// Indent is type + space + prefix + name + paren.
	indent := makeIndent(retTypeLen + namePrefixVisibleCharLen + len(fn.Name) + templateSpecCharLen + 1)
	for i, param := range fn.Params {
		if i > 0 {
			fmt.Fprintf(f, ",\n")
			f.Write(indent)
		}

		tn, _ := getEscapedTypeName(param.TypeRef.QualName)
		if len(param.Name) == 0 {
			// Unnamed parameter.
			fmt.Fprintf(f, "<span class=\"typ\">%s</span>", tn)
		} else {
			fmt.Fprintf(f, "<span class=\"typ\">%s</span> %s", tn, param.Name)
		}

		// Optional default parameter value.
		if param.DefaultValue != "" {
			fmt.Fprintf(f, " = %s", param.DefaultValue)
		}
	}

	fmt.Fprintf(f, ");\n")
}

// Writes the body of a function reference. This is used for both standalone functions and member
// functions.
//
// The |namePrefix| is prepended to the definition for defining class or namespace information.
// This could be extracted from the function but this lets the caller decide which information to
// include. The number of visible characters in this prefix is supplied, which allows the caller to
// include HTML tags and escaped characters while keeping things aligned.
func writeFunctionGroupBody(settings WriteSettings, index *Index, g *FunctionGroup,
	namePrefix string, namePrefixVisibleCharLen int,
	includeReturnType bool, f io.Writer) {
	// Use the first function's location for the definition location link.
	if g.Funcs[0].GetLocation().Filename != "" {
		fmt.Fprintf(f, "[Declaration source code](%s)\n\n",
			settings.locationSourceLink(g.Funcs[0].GetLocation()))
	}

	if !commentContains(g.Funcs[0].Description, NoDeclTag) {
		// Write the declaration.
		writePreHeader(f)
		for _, fn := range g.Funcs {
			writeFunctionDeclaration(fn, namePrefix, namePrefixVisibleCharLen,
				includeReturnType, "", f)
		}
		writePreFooter(f)
	}

	// Any comment is on the first function in the group. If it has a heading, it will have
	// been extracted and used as the title so we need to strip that to avoid duplicating.
	_, commentWithNoH1 := extractCommentHeading1(g.Funcs[0].Description)
	writeComment(index, commentWithNoH1, markdownHeading2, f)

	fmt.Fprintf(f, "\n")
}

func functionGroupHtmlId(g *FunctionGroup) string {
	// It seems devsite doesn't like more than one HTML ID for a heading. Until it is fixed
	// or we can find a workaround, just use the first function's ID.
	return functionHtmlId(g.Funcs[0])
}

// Writes the reference section for a standalone function.
func writeFunctionGroupSection(settings WriteSettings, index *Index, g *FunctionGroup, f io.Writer) {
	if g.ExplicitTitle != "" {
		fmt.Fprintf(f, "## %s {:#%s}\n\n", g.ExplicitTitle, functionGroupHtmlId(g))
	} else {
		fullName := functionFullName(g.Funcs[0])
		fmt.Fprintf(f, "## %s {:#%s}\n\n",
			titleWithTemplateSpecializations(fullName, g.Funcs[0].Template, functionGroupEllipsesParens(g), ""),
			functionGroupHtmlId(g))
	}

	// Include the qualified namespaces as a prefix.
	namespacePrefix := getScopeQualifier(g.Funcs[0].Namespace, true)
	writeFunctionGroupBody(settings, index, g, namespacePrefix, len(namespacePrefix), true, f)
}

// Interface for sorting a function list by function name.
type functionByName []*clangdoc.FunctionInfo

func (f functionByName) Len() int {
	return len(f)
}
func (f functionByName) Swap(i, j int) {
	f[i], f[j] = f[j], f[i]
}
func (f functionByName) Less(i, j int) bool {
	return f[i].Name < f[j].Name
}

// Interface for sorting a function list by declaration location.
type functionByLocation []*clangdoc.FunctionInfo

func (f functionByLocation) Len() int {
	return len(f)
}
func (f functionByLocation) Swap(i, j int) {
	f[i], f[j] = f[j], f[i]
}
func (f functionByLocation) Less(i, j int) bool {
	// This assumes the file names are the same (since we're normally processing by file).
	return f[i].GetLocation().LineNumber < f[j].GetLocation().LineNumber
}

func functionHtmlId(f *clangdoc.FunctionInfo) string {
	// Use the fully-qualified function name. This can still produce collisions due to
	// overloading but we don't have a way to differentiate this other than making all
	// references by USR, which makes manual linking impossible.
	//
	// TODO(fxbug.dev/119085) it would be nice to allow links to functions overloads and
	// template specializations (the same name). If this function took an Index parameter, it
	// could check for this case and use the USR id for all but the first instance.
	return getScopeQualifier(f.Namespace, true) + f.Name
}

func functionLink(f *clangdoc.FunctionInfo) string {
	return HeaderReferenceFile(f.GetLocation().Filename) + "#" + functionHtmlId(f)
}

func functionGroupLink(g *FunctionGroup) string {
	return HeaderReferenceFile(g.Funcs[0].GetLocation().Filename) + "#" + functionGroupHtmlId(g)
}

// Returns the fully-qualified name of a function.
func functionFullName(f *clangdoc.FunctionInfo) string {
	return getScopeQualifier(f.Namespace, true) + f.Name
}
