#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/RttiTool.h>

#include <Foundation/Reflection/ReflectionUtils.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpRttiTool, 1, plRTTIDefaultAllocator<plMcpRttiTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// The largest number of entries any of these tools will return. A project has well over a thousand
  /// reflected types, and an unfiltered query would otherwise fill the client's context with names it
  /// did not ask for. Every listing reports the total number of matches alongside, so the agent can
  /// tell 'this is all of them' from 'narrow your filter'.
  constexpr plUInt32 s_uiMaxResults = 200;

  bool NameMatches(const plRTTI* pType, plStringView sFilter)
  {
    return sFilter.IsEmpty() || pType->GetTypeName().FindSubString_NoCase(sFilter) != nullptr;
  }
} // namespace

void plMcpRttiTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "rtti_find_types";
    desc.m_sDescription = "Searches the reflected C++ types by name and returns only the matching type names. This is the entry "
                          "point into the reflection data: narrow down to a few names here, then use rtti_type_info or "
                          "rtti_type_properties on those. Results are capped, but the total number of matches is reported.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("name":{"type":"string","description":"Only return types whose name contains this text, case insensitive. Empty returns everything, which is a lot."},)"
                          R"("derivedFrom":{"type":"string","description":"Only return types deriving from this type, e.g. 'plComponent'."},)"
                          R"("concreteOnly":{"type":"boolean","description":"If true, exclude abstract types and types that cannot be allocated. Default false."})"
                          R"(}})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "rtti_type_info";
    desc.m_sDescription = "Returns the details of one reflected type: its parent type, the plugin it comes from, its type flags, "
                          "its attributes, and its callable functions with full signatures. Deliberately does NOT include the "
                          "property list - use rtti_type_properties for that.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("name":{"type":"string","description":"The exact type name, e.g. 'plMeshComponent'."})"
                          R"(},"required":["name"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "rtti_type_properties";
    desc.m_sDescription = "Returns the reflected properties of one type, with their value type, category (member, array, set, "
                          "map, constant), flags and attributes. This is what tells you the exact property names and value "
                          "types to use when reading or writing an object's properties.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("name":{"type":"string","description":"The exact type name, e.g. 'plMeshComponent'."},)"
                          R"("recursive":{"type":"boolean","description":"If true, also include the properties inherited from base types. Default false, which returns only the type's own properties."})"
                          R"(},"required":["name"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "rtti_derived_types";
    desc.m_sDescription = "Returns the names of all types deriving from the given type. Use this to answer questions like which "
                          "component types exist ('plComponent') or which types can go into a specific property.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("name":{"type":"string","description":"The exact base type name, e.g. 'plComponent'."},)"
                          R"("concreteOnly":{"type":"boolean","description":"If true, exclude abstract types and types that cannot be allocated. Default false."})"
                          R"(},"required":["name"]})";
  }
}

void plMcpRttiTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "rtti_find_types")
    ExecuteFindTypes(arguments, out_result);
  else if (sToolName == "rtti_type_info")
    ExecuteTypeInfo(arguments, out_result);
  else if (sToolName == "rtti_type_properties")
    ExecuteTypeProperties(arguments, out_result);
  else if (sToolName == "rtti_derived_types")
    ExecuteDerivedTypes(arguments, out_result);
}

void plMcpRttiTool::WriteTypeFlags(plMcpJsonWriter& ref_writer, const plRTTI* pType)
{
  const plBitflags<plTypeFlags>& flags = pType->GetTypeFlags();

  ref_writer.BeginArray("flags");

  if (flags.IsSet(plTypeFlags::StandardType))
    ref_writer.WriteString("StandardType");
  if (flags.IsSet(plTypeFlags::IsEnum))
    ref_writer.WriteString("IsEnum");
  if (flags.IsSet(plTypeFlags::Bitflags))
    ref_writer.WriteString("Bitflags");
  if (flags.IsSet(plTypeFlags::Class))
    ref_writer.WriteString("Class");
  if (flags.IsSet(plTypeFlags::Abstract))
    ref_writer.WriteString("Abstract");
  if (flags.IsSet(plTypeFlags::Phantom))
    ref_writer.WriteString("Phantom");
  if (flags.IsSet(plTypeFlags::Minimal))
    ref_writer.WriteString("Minimal");

  ref_writer.EndArray();
}

void plMcpRttiTool::WritePropertyFlags(plMcpJsonWriter& ref_writer, plStringView sName, plBitflags<plPropertyFlags> flags)
{
  // An empty flag array says nothing that its absence does not, and these repeat for every argument of
  // every function - which is exactly the token cost the split-tool design exists to avoid.
  if (flags.GetValue() == 0)
    return;

  ref_writer.BeginArray(sName);

  if (flags.IsSet(plPropertyFlags::StandardType))
    ref_writer.WriteString("StandardType");
  if (flags.IsSet(plPropertyFlags::IsEnum))
    ref_writer.WriteString("IsEnum");
  if (flags.IsSet(plPropertyFlags::Bitflags))
    ref_writer.WriteString("Bitflags");
  if (flags.IsSet(plPropertyFlags::Class))
    ref_writer.WriteString("Class");
  if (flags.IsSet(plPropertyFlags::Const))
    ref_writer.WriteString("Const");
  if (flags.IsSet(plPropertyFlags::Reference))
    ref_writer.WriteString("Reference");
  if (flags.IsSet(plPropertyFlags::Pointer))
    ref_writer.WriteString("Pointer");
  if (flags.IsSet(plPropertyFlags::PointerOwner))
    ref_writer.WriteString("PointerOwner");
  if (flags.IsSet(plPropertyFlags::ReadOnly))
    ref_writer.WriteString("ReadOnly");
  if (flags.IsSet(plPropertyFlags::Hidden))
    ref_writer.WriteString("Hidden");
  if (flags.IsSet(plPropertyFlags::Phantom))
    ref_writer.WriteString("Phantom");
  if (flags.IsSet(plPropertyFlags::VarOut))
    ref_writer.WriteString("VarOut");
  if (flags.IsSet(plPropertyFlags::VarInOut))
    ref_writer.WriteString("VarInOut");

  ref_writer.EndArray();
}

void plMcpRttiTool::WriteAttributes(plMcpJsonWriter& ref_writer, plArrayPtr<const plPropertyAttribute* const> attributes)
{
  if (attributes.IsEmpty())
    return;

  ref_writer.BeginArray("attributes");

  for (const plPropertyAttribute* pAttr : attributes)
  {
    if (pAttr == nullptr)
      continue;

    WriteAttribute(ref_writer, pAttr);
  }

  ref_writer.EndArray();
}

const plPropertyAttribute* plMcpRttiTool::GetAttributeFromVariant(const plVariant& value)
{
  if (value.GetType() != plVariant::Type::TypedPointer)
    return nullptr;

  const plTypedPointer ptr = value.Get<plTypedPointer>();

  if (ptr.m_pObject == nullptr || ptr.m_pType == nullptr)
    return nullptr;

  if (!ptr.m_pType->IsDerivedFrom<plPropertyAttribute>())
    return nullptr;

  return static_cast<const plPropertyAttribute*>(ptr.m_pObject);
}

void plMcpRttiTool::WriteAttribute(plMcpJsonWriter& ref_writer, const plPropertyAttribute* pAttr)
{
  {
    const plRTTI* pAttrType = pAttr->GetDynamicRTTI();

    ref_writer.BeginObject();
    ref_writer.AddVariableString("type", pAttrType->GetTypeName());

    // An attribute's own reflected members carry the interesting part - the clamp range of an
    // plClampValueAttribute, the type filter of an plAssetBrowserAttribute. They are read generically,
    // so attributes added later show up here without this tool knowing about them.
    for (const plAbstractProperty* pProp : pAttrType->GetProperties())
    {
      if (pProp->GetCategory() == plPropertyCategory::Member)
      {
        const plVariant value = plReflectionUtils::GetMemberPropertyValue(static_cast<const plAbstractMemberProperty*>(pProp), pAttr);

        if (value.IsValid())
        {
          ref_writer.AddVariableVariant(pProp->GetPropertyName(), value);
        }
      }
      else if (pProp->GetCategory() == plPropertyCategory::Array)
      {
        // Attributes such as plContainerAttribute keep their payload in an array. Skipping those would
        // silently drop the only interesting part of the attribute.
        const plAbstractArrayProperty* pArray = static_cast<const plAbstractArrayProperty*>(pProp);

        const plUInt32 uiCount = pArray->GetCount(pAttr);

        if (uiCount == 0)
          continue;

        ref_writer.BeginArray(pProp->GetPropertyName());

        for (plUInt32 uiIndex = 0; uiIndex < uiCount; ++uiIndex)
        {
          const plVariant value = plReflectionUtils::GetArrayPropertyValue(pArray, pAttr, uiIndex);

          // An attribute nested inside another one - plFunctionArgumentAttributes holds the attributes
          // of a single function argument this way. Writing it as a plain variant would report only its
          // type and drop the members, which for an plDefaultValueAttribute is the entire content.
          if (const plPropertyAttribute* pNested = GetAttributeFromVariant(value))
          {
            WriteAttribute(ref_writer, pNested);
          }
          else
          {
            ref_writer.WriteVariant(value);
          }
        }

        ref_writer.EndArray();
      }
    }

    ref_writer.EndObject();
  }
}

void plMcpRttiTool::WriteProperty(plMcpJsonWriter& ref_writer, const plAbstractProperty* pProp)
{
  ref_writer.BeginObject();

  ref_writer.AddVariableString("name", pProp->GetPropertyName());

  const plRTTI* pPropType = pProp->GetSpecificType();
  ref_writer.AddVariableString("type", pPropType != nullptr ? pPropType->GetTypeName() : plStringView());

  switch (pProp->GetCategory())
  {
    case plPropertyCategory::Constant:
      ref_writer.AddVariableString("category", "constant");
      break;
    case plPropertyCategory::Member:
      ref_writer.AddVariableString("category", "member");
      break;
    case plPropertyCategory::Function:
      ref_writer.AddVariableString("category", "function");
      break;
    case plPropertyCategory::Array:
      ref_writer.AddVariableString("category", "array");
      break;
    case plPropertyCategory::Set:
      ref_writer.AddVariableString("category", "set");
      break;
    case plPropertyCategory::Map:
      ref_writer.AddVariableString("category", "map");
      break;
    default:
      ref_writer.AddVariableString("category", "unknown");
      break;
  }

  const plBitflags<plPropertyFlags>& flags = pProp->GetFlags();

  WritePropertyFlags(ref_writer, "flags", flags);

  // For enum and bitflags properties the valid values are the constants of the value type, and without
  // them the agent has no way to know what may be assigned.
  if (flags.IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags) && pPropType != nullptr)
  {
    ref_writer.BeginArray("enumValues");

    for (const plAbstractProperty* pConstant : pPropType->GetProperties())
    {
      if (pConstant->GetCategory() != plPropertyCategory::Constant)
        continue;

      ref_writer.BeginObject();
      ref_writer.AddVariableString("name", pConstant->GetPropertyName());
      ref_writer.AddVariableVariant("value", static_cast<const plAbstractConstantProperty*>(pConstant)->GetConstant());
      ref_writer.EndObject();
    }

    ref_writer.EndArray();
  }

  WriteAttributes(ref_writer, pProp->GetAttributes());

  ref_writer.EndObject();
}

void plMcpRttiTool::WriteFunction(plMcpJsonWriter& ref_writer, const plAbstractFunctionProperty* pFunc)
{
  ref_writer.BeginObject();

  ref_writer.AddVariableString("name", pFunc->GetPropertyName());

  switch (pFunc->GetFunctionType())
  {
    case plFunctionType::Member:
      ref_writer.AddVariableString("functionType", "member");
      break;
    case plFunctionType::StaticMember:
      ref_writer.AddVariableString("functionType", "static");
      break;
    case plFunctionType::Constructor:
      ref_writer.AddVariableString("functionType", "constructor");
      break;
    default:
      ref_writer.AddVariableString("functionType", "unknown");
      break;
  }

  // A null return type means void, which is not a reflected type of its own.
  const plRTTI* pReturnType = pFunc->GetReturnType();
  ref_writer.AddVariableString("returnType", pReturnType != nullptr ? pReturnType->GetTypeName() : "void"_plsv);

  if (pReturnType != nullptr)
  {
    WritePropertyFlags(ref_writer, "returnFlags", pFunc->GetReturnFlags());
  }

  // Functions exposed to scripting carry the argument names, which the reflection data itself does not
  // hold - without them the arguments would only be identifiable by position.
  const plScriptableFunctionAttribute* pScriptable = pFunc->GetAttributeByType<plScriptableFunctionAttribute>();

  if (pScriptable != nullptr)
  {
    ref_writer.AddVariableBool("scriptable", true);
  }

  const plUInt32 uiArgCount = pFunc->GetArgumentCount();

  if (uiArgCount == 0)
  {
    WriteAttributes(ref_writer, pFunc->GetAttributes());
    ref_writer.EndObject();
    return;
  }

  ref_writer.BeginArray("arguments");
  for (plUInt32 uiArg = 0; uiArg < uiArgCount; ++uiArg)
  {
    ref_writer.BeginObject();

    if (pScriptable != nullptr && uiArg < pScriptable->GetArgumentCount())
    {
      ref_writer.AddVariableString("name", pScriptable->GetArgumentName(uiArg));
    }

    const plRTTI* pArgType = pFunc->GetArgumentType(uiArg);
    ref_writer.AddVariableString("type", pArgType != nullptr ? pArgType->GetTypeName() : plStringView());

    // The flags say whether the argument is an out or inout parameter, which decides whether a caller
    // has to pass a value in at all.
    WritePropertyFlags(ref_writer, "flags", pFunc->GetArgumentFlags(uiArg));

    ref_writer.EndObject();
  }
  ref_writer.EndArray();

  WriteAttributes(ref_writer, pFunc->GetAttributes());

  ref_writer.EndObject();
}

void plMcpRttiTool::ExecuteFindTypes(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sName = plMcpJson::GetString(arguments, "name");
  const plStringView sDerivedFrom = plMcpJson::GetString(arguments, "derivedFrom");
  const bool bConcreteOnly = plMcpJson::GetBool(arguments, "concreteOnly", false);

  const plRTTI* pBaseType = nullptr;

  if (!sDerivedFrom.IsEmpty())
  {
    pBaseType = plRTTI::FindTypeByName(sDerivedFrom);

    if (pBaseType == nullptr)
    {
      out_result.SetError(plStringBuilder("Unknown type '", sDerivedFrom, "' passed as 'derivedFrom'. Use rtti_find_types without it to search for the correct name."));
      return;
    }
  }

  const plBitflags<plRTTI::ForEachOptions> options = bConcreteOnly ? plRTTI::ForEachOptions::ExcludeNotConcrete : plRTTI::ForEachOptions::Default;

  plUInt32 uiTotalMatches = 0;
  plHybridArray<plStringView, 32> names;

  auto visitor = [&](const plRTTI* pType)
  {
    if (!NameMatches(pType, sName))
      return;

    ++uiTotalMatches;

    if (names.GetCount() < s_uiMaxResults)
    {
      names.PushBack(pType->GetTypeName());
    }
  };

  if (pBaseType != nullptr)
  {
    plRTTI::ForEachDerivedType(pBaseType, visitor, options);
  }
  else
  {
    plRTTI::ForEachType(visitor, options);
  }

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.AddVariableUInt32("totalMatches", uiTotalMatches);
  writer.AddVariableUInt32("returned", names.GetCount());
  writer.AddVariableBool("truncated", uiTotalMatches > names.GetCount());

  writer.BeginArray("types");
  for (plStringView sTypeName : names)
  {
    writer.WriteString(sTypeName);
  }
  writer.EndArray();

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpRttiTool::ExecuteTypeInfo(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sName = plMcpJson::GetString(arguments, "name");

  if (sName.IsEmpty())
  {
    out_result.SetError("Argument 'name' is required.");
    return;
  }

  const plRTTI* pType = plRTTI::FindTypeByName(sName);

  if (pType == nullptr)
  {
    out_result.SetError(plStringBuilder("Unknown type '", sName, "'. Type names are case sensitive - use rtti_find_types to search for the correct name."));
    return;
  }

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.AddVariableString("name", pType->GetTypeName());

  const plRTTI* pParent = pType->GetParentType();
  writer.AddVariableString("parentType", pParent != nullptr ? pParent->GetTypeName() : plStringView());

  writer.AddVariableString("plugin", pType->GetPluginName());
  writer.AddVariableUInt32("typeVersion", pType->GetTypeVersion());

  // Whether plRTTI can construct one. False for abstract types, and for types that are only registered
  // for their property information.
  const plRTTIAllocator* pAllocator = pType->GetAllocator();
  writer.AddVariableBool("canAllocate", pAllocator != nullptr && pAllocator->CanAllocate());

  WriteTypeFlags(writer, pType);

  // Counts rather than the lists themselves - this tool stays cheap on purpose, and the count is what
  // tells the agent whether calling rtti_type_properties is worth it.
  writer.AddVariableUInt32("numOwnProperties", pType->GetProperties().GetCount());

  plDynamicArray<const plAbstractProperty*> allProps;
  pType->GetAllProperties(allProps);
  writer.AddVariableUInt32("numAllProperties", allProps.GetCount());

  writer.BeginArray("functions");
  for (const plAbstractFunctionProperty* pFunc : pType->GetFunctions())
  {
    WriteFunction(writer, pFunc);
  }
  writer.EndArray();

  WriteAttributes(writer, pType->GetAttributes());

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpRttiTool::ExecuteTypeProperties(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sName = plMcpJson::GetString(arguments, "name");
  const bool bRecursive = plMcpJson::GetBool(arguments, "recursive", false);

  if (sName.IsEmpty())
  {
    out_result.SetError("Argument 'name' is required.");
    return;
  }

  const plRTTI* pType = plRTTI::FindTypeByName(sName);

  if (pType == nullptr)
  {
    out_result.SetError(plStringBuilder("Unknown type '", sName, "'. Type names are case sensitive - use rtti_find_types to search for the correct name."));
    return;
  }

  plDynamicArray<const plAbstractProperty*> properties;

  if (bRecursive)
  {
    // walks the base types as well, which is exactly what the argument asks for
    pType->GetAllProperties(properties);
  }
  else
  {
    properties = pType->GetProperties();
  }

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.AddVariableString("name", pType->GetTypeName());
  writer.AddVariableBool("recursive", bRecursive);

  writer.BeginArray("properties");
  for (const plAbstractProperty* pProp : properties)
  {
    WriteProperty(writer, pProp);
  }
  writer.EndArray();

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpRttiTool::ExecuteDerivedTypes(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sName = plMcpJson::GetString(arguments, "name");
  const bool bConcreteOnly = plMcpJson::GetBool(arguments, "concreteOnly", false);

  if (sName.IsEmpty())
  {
    out_result.SetError("Argument 'name' is required.");
    return;
  }

  const plRTTI* pType = plRTTI::FindTypeByName(sName);

  if (pType == nullptr)
  {
    out_result.SetError(plStringBuilder("Unknown type '", sName, "'. Type names are case sensitive - use rtti_find_types to search for the correct name."));
    return;
  }

  const plBitflags<plRTTI::ForEachOptions> options = bConcreteOnly ? plRTTI::ForEachOptions::ExcludeNotConcrete : plRTTI::ForEachOptions::Default;

  plUInt32 uiTotalMatches = 0;
  plHybridArray<plStringView, 32> names;

  plRTTI::ForEachDerivedType(pType,
    [&](const plRTTI* pDerived)
    {
      // ForEachDerivedType includes the base type itself, which is not what 'derived types' means here
      if (pDerived == pType)
        return;

      ++uiTotalMatches;

      if (names.GetCount() < s_uiMaxResults)
      {
        names.PushBack(pDerived->GetTypeName());
      }
    },
    options);

  plMcpJsonWriter writer;
  writer.BeginObject();

  writer.AddVariableString("baseType", pType->GetTypeName());
  writer.AddVariableUInt32("totalMatches", uiTotalMatches);
  writer.AddVariableUInt32("returned", names.GetCount());
  writer.AddVariableBool("truncated", uiTotalMatches > names.GetCount());

  writer.BeginArray("types");
  for (plStringView sTypeName : names)
  {
    writer.WriteString(sTypeName);
  }
  writer.EndArray();

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}
