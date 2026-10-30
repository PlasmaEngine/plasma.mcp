#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

class plRTTI;
class plMcpJsonWriter;
class plAbstractProperty;
class plAbstractFunctionProperty;

/// \brief Exposes the reflection data, so an agent can find out which types exist and what they look like.
///
/// Split into four narrow tools rather than one, because the codebase has well over a thousand reflected
/// types: a single 'dump everything' call would cost more tokens than a client can spend and would bury
/// whatever was actually asked for. The intended flow is rtti_find_types to narrow down to a few names,
/// then the detail tools for those.
///
/// Editor-side phantom types are included automatically - plPhantomRttiManager registers them with plRTTI,
/// so they are visible to the same traversal as the native ones.
class plMcpRttiTool : public plMcpToolProvider
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpRttiTool, plMcpToolProvider);

public:
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const override;
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) override;

private:
  void ExecuteFindTypes(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteTypeInfo(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteTypeProperties(const plVariantDictionary& arguments, plMcpToolResult& out_result);
  void ExecuteDerivedTypes(const plVariantDictionary& arguments, plMcpToolResult& out_result);

  static void WriteTypeFlags(plMcpJsonWriter& ref_writer, const plRTTI* pType);
  static void WritePropertyFlags(plMcpJsonWriter& ref_writer, plStringView sName, plBitflags<plPropertyFlags> flags);
  static void WriteAttributes(plMcpJsonWriter& ref_writer, plArrayPtr<const plPropertyAttribute* const> attributes);

  /// Writes one attribute as an object: its concrete type plus its reflected members.
  static void WriteAttribute(plMcpJsonWriter& ref_writer, const plPropertyAttribute* pAttr);

  /// Returns the attribute a variant points at, or nullptr if it does not hold one. Attributes nested
  /// inside another attribute arrive as a TypedPointer and would otherwise lose their contents.
  static const plPropertyAttribute* GetAttributeFromVariant(const plVariant& value);
  static void WriteProperty(plMcpJsonWriter& ref_writer, const plAbstractProperty* pProp);
  static void WriteFunction(plMcpJsonWriter& ref_writer, const plAbstractFunctionProperty* pFunc);
};
