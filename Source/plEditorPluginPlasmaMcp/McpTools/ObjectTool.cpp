#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/ObjectTool.h>

#include <Foundation/Containers/Deque.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <ToolsFoundation/Document/DocumentManager.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <ToolsFoundation/Object/ObjectAccessorBase.h>
#include <ToolsFoundation/Selection/SelectionManager.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpObjectTool, 1, plRTTIDefaultAllocator<plMcpObjectTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// Resolves the 'document' argument to an open document. See plMcpToolProvider::FindOpenDocument().
  plDocument* ObjectToolFindDocument(plStringView sIdentifier)
  {
    return plMcpToolProvider::FindOpenDocument(sIdentifier);
  }

  /// Returns the object the tools act on: the one named by 'object', or the document's top level
  /// object when that argument is missing.
  ///
  /// The top level object is the root's single child. That is what plSimpleAssetDocument's
  /// GetPropertyObject() returns, but reproduced here through the generic plDocumentObject interface,
  /// because GetPropertyObject() is declared on the template and cannot be called without knowing the
  /// asset type. Documents that hold several top level objects - scenes - have no single answer, and
  /// the caller has to name one.
  const plDocumentObject* ObjectToolResolveObject(const plDocument* pDocument, plStringView sObjectGuid, plStringBuilder& out_sError)
  {
    const plDocumentObjectManager* pManager = pDocument->GetObjectManager();

    // The root itself holds no properties, but it is the parent every top level object hangs off, so it
    // is the only way to add one to a scene. Named rather than addressed by guid because a caller that
    // could name the root's guid would already have had to enumerate the document.
    if (sObjectGuid.IsEqual_NoCase("root"))
    {
      return pManager->GetRootObject();
    }

    if (!sObjectGuid.IsEmpty())
    {
      if (!plConversionUtils::IsStringUuid(sObjectGuid))
      {
        out_sError.SetFormat("'{}' is not a valid object guid. Omit 'object' to address the document's top level object, or pass "
                             "'root' to address the document root itself.",
          sObjectGuid);
        return nullptr;
      }

      const plUuid guid = plConversionUtils::ConvertStringToUuid(sObjectGuid);
      const plDocumentObject* pObject = pManager->GetObject(guid);

      if (pObject == nullptr)
      {
        out_sError.SetFormat("The document has no object with guid '{}'. Object guids come from a previous 'object_properties' call "
                             "and are not stable across editor sessions.",
          sObjectGuid);
      }

      return pObject;
    }

    const plDocumentObject* pRoot = pManager->GetRootObject();

    if (pRoot == nullptr || pRoot->GetChildren().IsEmpty())
    {
      out_sError = "The document has no top level object.";
      return nullptr;
    }

    if (pRoot->GetChildren().GetCount() > 1)
    {
      out_sError.SetFormat("The document has {} top level objects, so there is no single one to default to. Use 'object_tree' to list "
                           "them and pass 'object' with the guid of the one to act on, or pass 'root' to address the document root "
                           "itself - that is the parent to add a new top level object to.",
        pRoot->GetChildren().GetCount());
      return nullptr;
    }

    return pRoot->GetChildren()[0];
  }

  plStringView ObjectToolCategoryToString(plPropertyCategory::Enum category)
  {
    // Deliberately the same words rtti_type_properties reports, so an agent that looked a type up
    // does not have to map one vocabulary onto another.
    switch (category)
    {
      case plPropertyCategory::Constant: return "constant";
      case plPropertyCategory::Member: return "member";
      case plPropertyCategory::Function: return "function";
      case plPropertyCategory::Array: return "array";
      case plPropertyCategory::Set: return "set";
      case plPropertyCategory::Map: return "map";
      default: return "unknown";
    }
  }

  /// The name an object shows in the scene tree, or an empty view for a type that has no Name property.
  ///
  /// Components do not have one - they are identified by their type - so an empty result is normal and
  /// means 'this object has no name', not 'the name could not be read'.
  plStringView ObjectToolGetObjectName(const plDocumentObject* pObject, plStringBuilder& ref_sStorage)
  {
    const plAbstractProperty* pProp = pObject->GetType()->FindPropertyByName("Name");

    if (pProp == nullptr || pProp->GetCategory() != plPropertyCategory::Member)
      return {};

    const plVariant value = pObject->GetTypeAccessor().GetValue("Name");

    if (!value.IsA<plString>())
      return {};

    ref_sStorage = value.Get<plString>();
    return ref_sStorage;
  }

  /// Writes one object and, within the limits, everything below it.
  ///
  /// Both limits exist because a scene is unbounded: a full dump of a real level is far more than a
  /// caller asked for and more than its context can hold. Hitting either is reported per node rather
  /// than silently, so 'no children' and 'children not walked' stay distinguishable.
  void ObjectToolWriteTreeNode(plMcpJsonWriter& ref_writer, const plDocumentObject* pObject, plUInt32 uiDepth, plUInt32 uiMaxDepth,
    plUInt32& ref_uiRemainingNodes)
  {
    plStringBuilder sName;

    ref_writer.BeginObject();
    ref_writer.AddVariableUuid("guid", pObject->GetGuid());
    ref_writer.AddVariableString("type", pObject->GetType()->GetTypeName());

    const plStringView sObjectName = ObjectToolGetObjectName(pObject, sName);

    if (!sObjectName.IsEmpty())
      ref_writer.AddVariableString("name", sObjectName);

    // Which property of the parent holds this object. A scene object sits in 'Children' and a component
    // in 'Components', so this is what tells the two apart, and it is what 'object_reparent' needs.
    if (pObject->GetParent() != nullptr && !pObject->GetParentProperty().IsEmpty())
      ref_writer.AddVariableString("property", pObject->GetParentProperty());

    const plUInt32 uiChildCount = pObject->GetChildren().GetCount();

    if (uiChildCount > 0)
    {
      ref_writer.AddVariableUInt32("childCount", uiChildCount);

      if (uiDepth >= uiMaxDepth)
      {
        ref_writer.AddVariableBool("childrenNotWalked", true);
      }
      else
      {
        ref_writer.BeginArray("children");

        for (const plDocumentObject* pChild : pObject->GetChildren())
        {
          if (ref_uiRemainingNodes == 0)
          {
            ref_writer.EndArray();
            ref_writer.AddVariableBool("childrenTruncated", true);
            ref_writer.EndObject();
            return;
          }

          --ref_uiRemainingNodes;
          ObjectToolWriteTreeNode(ref_writer, pChild, uiDepth + 1, uiMaxDepth, ref_uiRemainingNodes);
        }

        ref_writer.EndArray();
      }
    }

    ref_writer.EndObject();
  }

  /// Reads the components of a vector, quaternion or colour out of a tool argument.
  ///
  /// Accepts both the object form these types are reported in ({"x":1,"y":2,"z":3} and
  /// {"r":1,"g":1,"b":1,"a":1}) and a plain array ([1,2,3]). plVariant converts neither on its own, so
  /// without this no vector, quaternion or colour property could be set at all - which rules out
  /// position, rotation and scale, and therefore most of what a scene object is.
  ///
  /// Returns how many components were read, stopping at the first one missing. The caller checks that
  /// against what its target type needs, which is what makes a colour's alpha optional.
  /// Parses a JSON object or array that arrived as text.
  ///
  /// 'value' is polymorphic, so its schema can declare no type - and a client given no type sends the
  /// argument as a string, so a vector written as {"x":1,"y":2,"z":3} arrives as those 21 characters
  /// rather than as an object. Returns an invalid variant for anything that is not JSON.
  plVariant ObjectToolParseJsonText(plStringView sText)
  {
    plStringBuilder sTrimmed = sText;
    sTrimmed.Trim(" \t\r\n");

    // Checked before parsing so that ordinary strings - which are the common case - never reach the
    // parser and never log a failure for being what they are.
    if (!sTrimmed.StartsWith("{") && !sTrimmed.StartsWith("["))
      return plVariant();

    plRawMemoryStreamReader reader(sTrimmed.GetData(), sTrimmed.GetElementCount());

    plJSONReader json;
    if (json.Parse(reader).Failed())
      return plVariant();

    if (json.GetTopLevelElementType() == plJSONReader::ElementType::Array)
      return plVariant(json.GetTopLevelArray());

    return plVariant(json.GetTopLevelObject());
  }

  plUInt32 ObjectToolReadComponents(const plVariant& rawInput, bool bColor, float* out_pValues)
  {
    const char* szNames[4] = {"x", "y", "z", "w"};

    if (bColor)
    {
      szNames[0] = "r";
      szNames[1] = "g";
      szNames[2] = "b";
      szNames[3] = "a";
    }

    // Text first, because that is how these values actually arrive - see ObjectToolParseJsonText().
    plVariant parsed;

    if (rawInput.IsA<plString>() || rawInput.IsA<plStringView>())
      parsed = ObjectToolParseJsonText(rawInput.ConvertTo<plString>());

    const plVariant& input = parsed.IsValid() ? parsed : rawInput;

    if (input.IsA<plVariantArray>())
    {
      const plVariantArray& values = input.Get<plVariantArray>();
      const plUInt32 uiCount = plMath::Min(values.GetCount(), 4u);

      for (plUInt32 i = 0; i < uiCount; ++i)
      {
        if (!values[i].CanConvertTo<float>())
          return i;

        out_pValues[i] = values[i].ConvertTo<float>();
      }

      return uiCount;
    }

    if (input.IsA<plVariantDictionary>())
    {
      const plVariantDictionary& dict = input.Get<plVariantDictionary>();

      for (plUInt32 i = 0; i < 4; ++i)
      {
        auto it = dict.Find(szNames[i]);

        if (!it.IsValid() || !it.Value().CanConvertTo<float>())
          return i;

        out_pValues[i] = it.Value().ConvertTo<float>();
      }

      return 4;
    }

    return 0;
  }

  /// Builds one of the composite standard types from a tool argument, or leaves out_value invalid if
  /// the target is not composite or the value does not carry enough components for it.
  void ObjectToolCoerceComposite(const plVariant& input, plVariant::Type::Enum targetType, plVariant& out_value)
  {
    const bool bColor = targetType == plVariant::Type::Color || targetType == plVariant::Type::ColorGamma;

    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const plUInt32 uiRead = ObjectToolReadComponents(input, bColor, v);

    switch (targetType)
    {
      case plVariant::Type::Vector2:
        if (uiRead >= 2)
          out_value = plVec2(v[0], v[1]);
        break;

      case plVariant::Type::Vector3:
        if (uiRead >= 3)
          out_value = plVec3(v[0], v[1], v[2]);
        break;

      case plVariant::Type::Vector4:
        if (uiRead >= 4)
          out_value = plVec4(v[0], v[1], v[2], v[3]);
        break;

      case plVariant::Type::Quaternion:
        if (uiRead >= 4)
          out_value = plQuat(v[0], v[1], v[2], v[3]);
        break;

      // Alpha is optional on both colours, because an opaque colour is what a caller almost always
      // means and writing {"r":1,"g":0,"b":0} should not fail for want of it.
      case plVariant::Type::Color:
        if (uiRead >= 3)
          out_value = plColor(v[0], v[1], v[2], v[3]);
        break;

      case plVariant::Type::ColorGamma:
        if (uiRead >= 3)
        {
          // Reported as 0-255 by the writer, so that is the range accepted back.
          out_value = plColorGammaUB(static_cast<plUInt8>(v[0]), static_cast<plUInt8>(v[1]), static_cast<plUInt8>(v[2]),
            uiRead >= 4 ? static_cast<plUInt8>(v[3]) : static_cast<plUInt8>(255));
        }
        break;

      default:
        break;
    }
  }

  /// Turns a value from the tool arguments into a variant the accessor will accept for this property.
  ///
  /// The JSON parser gives every number as a double and an AI client sends numbers as strings often
  /// enough that a straight type check would reject correct intent. Enums and bitflags additionally
  /// arrive as their names, which is the only form an agent can read out of rtti_type_properties.
  plResult ObjectToolCoerceValue(const plVariant& input, const plAbstractProperty* pProp, plVariant& out_value, plStringBuilder& out_sError)
  {
    const plRTTI* pPropType = pProp->GetSpecificType();
    const plBitflags<plPropertyFlags> flags = pProp->GetFlags();

    if (flags.IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags) && pPropType != nullptr)
    {
      // A name, possibly fully qualified. Numbers are still accepted, because a round trip through
      // this tool reports the name but other sources report the value.
      if (input.IsA<plString>() || input.IsA<plStringView>())
      {
        const plString sValue = input.ConvertTo<plString>();

        plInt64 iValue = 0;
        if (plReflectionUtils::StringToEnumeration(pPropType, sValue, iValue))
        {
          out_value = iValue;
          return PL_SUCCESS;
        }

        out_sError.SetFormat("'{}' is not a valid value for '{}'. 'rtti_type_properties' lists the valid names under 'enumValues'.",
          sValue, pProp->GetPropertyName());
        return PL_FAILURE;
      }

      if (input.CanConvertTo<plInt64>())
      {
        out_value = input.ConvertTo<plInt64>();
        return PL_SUCCESS;
      }

      out_sError.SetFormat("'{}' expects an enum name or a number.", pProp->GetPropertyName());
      return PL_FAILURE;
    }

    // For a plain member the property's own type says what is wanted. Converting rather than
    // requiring an exact match is what lets 42 arrive as a double and still set an plInt32.
    if (pPropType != nullptr && flags.IsSet(plPropertyFlags::StandardType))
    {
      const plVariant::Type::Enum targetType = pPropType->GetVariantType();

      if (targetType != plVariant::Type::Invalid && input.GetType() != targetType)
      {
        // plVariant has no string conversion for plUuid, but a guid string is how every tool here
        // names an object or an asset, so it has to be accepted wherever a plUuid is wanted - node
        // graph connections, for one, address their endpoints that way.
        if (targetType == plVariant::Type::Uuid && (input.IsA<plString>() || input.IsA<plStringView>()))
        {
          const plString sValue = input.ConvertTo<plString>();

          plUuid uuid;
          if (plConversionUtils::TryConvertStringToUuid(sValue, uuid))
          {
            out_value = uuid;
            return PL_SUCCESS;
          }

          out_sError.SetFormat("'{}' is not a valid guid for '{}'. Object guids come from 'object_tree', asset guids from 'asset_find'.",
            sValue, pProp->GetPropertyName());
          return PL_FAILURE;
        }

        // Angles arrive as a plain number in degrees, the unit the property grid shows.
        if (targetType == plVariant::Type::Angle && input.IsNumber())
        {
          out_value = plAngle::MakeFromDegree(input.ConvertTo<float>());
          return PL_SUCCESS;
        }

        // Vectors, quaternions and colours arrive as a JSON object or array, which plVariant has no
        // conversion for, so they are assembled component by component before anything else is tried.
        plVariant composite;
        ObjectToolCoerceComposite(input, targetType, composite);

        if (composite.IsValid())
        {
          out_value = composite;
          return PL_SUCCESS;
        }

        if (!input.CanConvertTo(targetType))
        {
          // No literal braces in this text: plFormatString does not escape them, so a spelled out
          // example object would come back through the formatter mangled.
          out_sError.SetFormat("The value given for '{}' cannot be converted to {}. Vectors and quaternions take an object with x, y, "
                               "z and w members, or an array such as [1,2,3]; colours use r, g, b and an optional a.",
            pProp->GetPropertyName(), pPropType->GetTypeName());
          return PL_FAILURE;
        }

        plResult conversion = PL_FAILURE;
        out_value = input.ConvertTo(targetType, &conversion);

        if (conversion.Failed())
        {
          out_sError.SetFormat("The value given for '{}' could not be converted to {}.", pProp->GetPropertyName(), pPropType->GetTypeName());
          return PL_FAILURE;
        }

        return PL_SUCCESS;
      }
    }

    out_value = input;
    return PL_SUCCESS;
  }

  /// Reads the 'value' argument, which unlike the others may legitimately be of any type.
  const plVariant* ObjectToolGetRawValue(const plVariantDictionary& arguments, plStringView sKey)
  {
    const plVariant* pValue = nullptr;
    if (arguments.TryGetValue(sKey, pValue))
      return pValue;

    return nullptr;
  }

  /// Checks that an index actually addresses an existing element.
  ///
  /// The accessors hand the index straight to reflection without validating it, where an out of range
  /// array index asserts - which kills the editor rather than failing the call. So every index that
  /// reaches a container has to be checked here first. 'bAllowEnd' is for insertion, where appending
  /// one past the last element is the normal case.
  plResult ObjectToolValidateIndex(plObjectAccessorBase* pAccessor, const plDocumentObject* pObject, const plAbstractProperty* pProp,
    const plVariant& index, bool bAllowEnd, plStringBuilder& out_sError)
  {
    const plPropertyCategory::Enum category = pProp->GetCategory();

    if (category == plPropertyCategory::Map)
    {
      // A map key that does not exist is a lookup miss rather than an out of bounds access, but the
      // same reasoning applies: report it here instead of finding out deeper down.
      if (bAllowEnd)
        return PL_SUCCESS;

      plDynamicArray<plVariant> keys;
      if (pAccessor->GetKeysByName(pObject, pProp->GetPropertyName(), keys).Failed())
      {
        out_sError.SetFormat("Could not read the keys of '{}', so a key cannot be used with it safely.", pProp->GetPropertyName());
        return PL_FAILURE;
      }

      const plString sKey = index.ConvertTo<plString>();

      for (const plVariant& key : keys)
      {
        if (key.ConvertTo<plString>() == sKey)
          return PL_SUCCESS;
      }

      out_sError.SetFormat("'{}' has no key '{}'. 'object_properties' lists the keys it has.", pProp->GetPropertyName(), sKey);
      return PL_FAILURE;
    }

    if (category != plPropertyCategory::Array && category != plPropertyCategory::Set)
      return PL_SUCCESS;

    plInt32 iCount = 0;
    if (pAccessor->GetCountByName(pObject, pProp->GetPropertyName(), iCount).Failed())
    {
      // Refusing rather than passing the index through: the reflection layer asserts on an out of
      // range index, and an assert is a dead editor. Without a count there is no way to know the
      // index is safe, so the only correct answer is not to try.
      out_sError.SetFormat("Could not determine how many elements '{}' has, so an index cannot be used with it safely.",
        pProp->GetPropertyName());
      return PL_FAILURE;
    }

    const plInt64 iIndex = index.ConvertTo<plInt64>();
    const plInt64 iLimit = bAllowEnd ? iCount : iCount - 1;

    if (iIndex < 0 || iIndex > iLimit)
    {
      if (iCount == 0)
      {
        out_sError.SetFormat("'{}' is empty, so there is no element {}.", pProp->GetPropertyName(), iIndex);
      }
      else
      {
        out_sError.SetFormat("Index {} is out of range for '{}', which has {} element(s), so the valid range is 0 to {}.",
          iIndex, pProp->GetPropertyName(), iCount, iLimit);
      }

      return PL_FAILURE;
    }

    return PL_SUCCESS;
  }

  /// Turns the 'index' argument into what the accessor expects: an plUInt32 for arrays and sets, a
  /// string key for maps. Returns an invalid variant when no index was given.
  plVariant ObjectToolGetIndex(const plVariantDictionary& arguments, plPropertyCategory::Enum category)
  {
    const plVariant* pIndex = ObjectToolGetRawValue(arguments, "index");

    if (pIndex == nullptr || !pIndex->IsValid())
      return plVariant();

    if (category == plPropertyCategory::Map)
      return plVariant(pIndex->ConvertTo<plString>());

    if (pIndex->CanConvertTo<plUInt32>())
      return plVariant(pIndex->ConvertTo<plUInt32>());

    return plVariant();
  }
} // namespace

void plMcpObjectTool::WritePropertyValue(plMcpJsonWriter& ref_writer, plObjectAccessorBase* pAccessor, const plDocumentObject* pObject,
  const plAbstractProperty* pProp, bool bIncludeValues)
{
  ref_writer.BeginObject();
  ref_writer.AddVariableString("name", pProp->GetPropertyName());

  const plRTTI* pPropType = pProp->GetSpecificType();
  ref_writer.AddVariableString("type", pPropType != nullptr ? pPropType->GetTypeName() : plStringView());

  const plPropertyCategory::Enum category = pProp->GetCategory();
  ref_writer.AddVariableString("category", ObjectToolCategoryToString(category));

  const plBitflags<plPropertyFlags> flags = pProp->GetFlags();

  // Only the flags that change what a caller may do with the property. The full set is available from
  // rtti_type_properties and would be repeated noise on every value.
  if (flags.IsSet(plPropertyFlags::ReadOnly))
    ref_writer.AddVariableBool("readOnly", true);

  if (flags.IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags))
    ref_writer.AddVariableBool("isEnum", true);

  if (!bIncludeValues)
  {
    ref_writer.EndObject();
    return;
  }

  switch (category)
  {
    case plPropertyCategory::Member:
    {
      // A member whose value is an embedded object reports that object's guid, not its contents: the
      // tree is walked with follow up calls, so one response stays bounded regardless of nesting.
      if (flags.IsAnySet(plPropertyFlags::Class | plPropertyFlags::Pointer) && !flags.IsSet(plPropertyFlags::StandardType))
      {
        const plDocumentObject* pChild = pAccessor->GetChildObjectByName(pObject, pProp->GetPropertyName(), plVariant());

        if (pChild != nullptr)
        {
          plStringBuilder sChildGuid;
          plConversionUtils::ToString(pChild->GetGuid(), sChildGuid);
          ref_writer.AddVariableString("objectGuid", sChildGuid);
          ref_writer.AddVariableString("objectType", pChild->GetType() != nullptr ? pChild->GetType()->GetTypeName() : plStringView());
        }
        else
        {
          // Distinguishes 'the pointer is null' from 'this property has no value', which for an
          // optional sub object is the interesting difference.
          ref_writer.AddVariableBool("null", true);
        }

        break;
      }

      plVariant value;
      const plStatus res = pAccessor->GetValueByName(pObject, pProp->GetPropertyName(), value);

      if (res.Failed())
      {
        ref_writer.AddVariableString("error", res.GetMessageString());
        break;
      }

      // An enum's number means nothing to a reader, and the name is what has to be passed back in.
      if (flags.IsAnySet(plPropertyFlags::IsEnum | plPropertyFlags::Bitflags) && pPropType != nullptr && value.CanConvertTo<plInt64>())
      {
        plStringBuilder sName;
        if (plReflectionUtils::EnumerationToString(pPropType, value.ConvertTo<plInt64>(), sName, plReflectionUtils::EnumConversionMode::ValueNameOnly))
        {
          ref_writer.AddVariableString("value", sName);
          break;
        }
      }

      ref_writer.BeginVariable("value");
      ref_writer.WriteVariant(value);
      ref_writer.EndVariable();
      break;
    }

    case plPropertyCategory::Array:
    case plPropertyCategory::Set:
    {
      plInt32 iCount = 0;
      if (pAccessor->GetCountByName(pObject, pProp->GetPropertyName(), iCount).Succeeded())
        ref_writer.AddVariableInt32("count", iCount);

      break;
    }

    case plPropertyCategory::Map:
    {
      plDynamicArray<plVariant> keys;
      if (pAccessor->GetKeysByName(pObject, pProp->GetPropertyName(), keys).Succeeded())
      {
        ref_writer.AddVariableInt32("count", keys.GetCount());

        // Keys, unlike elements, are what a caller needs in order to ask for anything at all.
        ref_writer.BeginArray("keys");
        for (const plVariant& key : keys)
        {
          ref_writer.WriteString(key.ConvertTo<plString>());
        }
        ref_writer.EndArray();
      }
      break;
    }

    default:
      break;
  }

  ref_writer.EndObject();
}

void plMcpObjectTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "object_properties";
    desc.m_sDescription =
      "Reads the properties of one object inside an open document, with their current values. With no 'object' argument this is the "
      "document's top level object, which for most asset types is the single object holding all of the asset's settings - so reading "
      "an asset's configuration is one call with just 'document'. "
      "Only direct properties are returned: a property whose value is another object reports that object's guid under 'objectGuid', "
      "and arrays, sets and maps report a count (and, for maps, their keys) rather than their elements. Pass 'property' with an "
      "'index' to read one element. The document has to be open - use 'document_open' with 'focus' false to open it invisibly.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open document. 'document_list' reports both."},)"
                          R"("object":{"type":"string","description":"Guid of the object to read, as reported by a previous call. Omit for the document's top level object."},)"
                          R"("property":{"type":"string","description":"Only read this one property. Combine with 'index' to read a single container element."},)"
                          R"("index":{"description":"Element to read from an array or set (a number) or from a map (the key). Only valid together with 'property'."},)"
                          R"("includeValues":{"type":"boolean","description":"If false, only report the property names and types, not their values. Defaults to true."})"
                          R"(},"required":["document"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "object_tree";
    desc.m_sDescription = "Lists the objects inside an open document as a tree: guid, type, name, and which property of its parent "
                          "each one sits in. This is where object guids come from, and therefore the first call to make against a "
                          "scene - every other object tool needs a guid, and a scene has many top level objects rather than the "
                          "single one the other tools default to. Components appear alongside child objects, told apart by their "
                          "'property' being 'Components' rather than 'Children'. Reads only.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open document."},)"
                          R"("object":{"type":"string","description":"Guid of the object to start from. Omit to list the whole document from its root."},)"
                          R"("depth":{"type":"number","description":"How many levels below the starting point to walk. Default 3. A node whose children were not walked says so."},)"
                          R"("maxNodes":{"type":"number","description":"Stop after this many objects. Default 300, which keeps a large scene from filling the reply."})"
                          R"(},"required":["document"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "object_modify";
    desc.m_sDescription =
      "Changes a property of an object inside an open document. Each call is one undoable step, exactly as if a user had made the "
      "change in the editor, and shows up in the undo menu labelled as coming from MCP. "
      "The document is NOT saved - call 'document_save' when the change is meant to be permanent, which is also what makes it "
      "visible to the asset system. Undo it with 'object_undo'. "
      "Operations: 'set' writes a value (for an array or map element, pass 'index'); 'insert' adds a value to a container; 'remove' "
      "deletes a container element; 'move' reorders one; 'addObject' creates an embedded object of a given type and 'removeObject' "
      "deletes one. Enum properties take the value name, as reported by 'rtti_type_properties'.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open document."},)"
                          R"("object":{"type":"string","description":"Guid of the object to modify. Omit for the document's top level object."},)"
                          R"("property":{"type":"string","description":"Name of the property to change. See 'object_properties'."},)"
                          R"("operation":{"type":"string","enum":["set","insert","remove","move","addObject","removeObject"],"description":"What to do. Defaults to 'set'."},)"
                          R"("value":{"description":"The new value, for 'set' and 'insert'. Enums take the value name as a string."},)"
                          R"("index":{"description":"Which element: a number for arrays and sets, the key for maps. Required for 'remove' and for 'set' on a container."},)"
                          R"("newIndex":{"description":"Where to move the element to, for 'move'."},)"
                          R"("type":{"type":"string","description":"The type to instantiate, for 'addObject'. Must be compatible with the property."})"
                          R"(},"required":["document"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "object_delete";
    desc.m_sDescription = "Deletes one object, named by its own guid, along with everything under it. "
                          "'object_modify' can also remove an object, but only by naming its parent, the property holding it and its "
                          "index - use this instead whenever you already have the object's guid. Undoable through 'object_undo'.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open document."},)"
                          R"("object":{"type":"string","description":"Guid of the object to delete."})"
                          R"(},"required":["document","object"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "object_reparent";
    desc.m_sDescription = "Moves an object to a different parent, or to the top level of the document. In a scene this is what "
                          "attaching one game object to another does, and it is the one structural edit 'object_modify' cannot "
                          "express - its 'move' reorders a value inside one container, it does not move an object between parents. "
                          "Undoable through 'object_undo'.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open document."},)"
                          R"("object":{"type":"string","description":"Guid of the object to move."},)"
                          R"("newParent":{"type":"string","description":"Guid of the object to move it under. Omit to move it to the top level of the document, i.e. detach it from its current parent."},)"
                          R"("property":{"type":"string","description":"The property on the new parent that receives it, e.g. 'Children'. Defaults to the property the object currently sits in, which is what you want for a scene."},)"
                          R"("index":{"type":"number","description":"Where among the new parent's children to put it. Defaults to appending at the end."})"
                          R"(},"required":["document","object"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "object_select";
    desc.m_sDescription = "Reads or changes which objects are selected in an open document. This is the editor's real selection, the "
                          "one shown in the scene tree and driving the property panel, so it is how an agent and the user refer to "
                          "the same objects: read it to find out what the user means by 'this object', set it to show them what you "
                          "changed. Always returns the resulting selection. Selection is not part of the undo stack.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open document."},)"
                          R"("action":{"type":"string","enum":["get","set","add","remove","clear"],"description":"Defaults to 'get' when no objects are given and 'set' when they are."},)"
                          R"("objects":{"type":"array","items":{"type":"string"},"description":"Object guids, for 'set', 'add' and 'remove'."})"
                          R"(},"required":["document"]})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "object_undo";
    desc.m_sDescription =
      "Undoes or redoes changes in an open document, and reports what is on its undo stack. This is the same stack the user's undo "
      "menu shows, so it also undoes changes the user made. The stack lives on the open document and is lost when the document is "
      "closed. Call with no 'action' to only look.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("document":{"type":"string","description":"Guid or path of an open document."},)"
                          R"("action":{"type":"string","enum":["undo","redo"],"description":"Omit to only report the current stack."},)"
                          R"("count":{"type":"number","description":"How many steps to undo or redo. Defaults to 1."})"
                          R"(},"required":["document"]})";
  }
}

void plMcpObjectTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "object_properties")
    ExecuteReadProperties(arguments, out_result);
  else if (sToolName == "object_tree")
    ExecuteTree(arguments, out_result);
  else if (sToolName == "object_modify")
    ExecuteModify(arguments, out_result);
  else if (sToolName == "object_delete")
    ExecuteDelete(arguments, out_result);
  else if (sToolName == "object_reparent")
    ExecuteReparent(arguments, out_result);
  else if (sToolName == "object_select")
    ExecuteSelect(arguments, out_result);
  else if (sToolName == "object_undo")
    ExecuteUndo(arguments, out_result);
}

void plMcpObjectTool::ExecuteReadProperties(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sDocument = plMcpJson::GetString(arguments, "document");

  plDocument* pDocument = ObjectToolFindDocument(sDocument);

  if (pDocument == nullptr)
  {
    plStringBuilder s;
    s.SetFormat("No open document matches '{}'. Open it first with 'document_open' - documents are never opened implicitly, because "
                "that would leave them open invisibly. 'document_list' reports what is open.",
      sDocument);
    out_result.SetError(s);
    return;
  }

  plStringBuilder sError;
  const plDocumentObject* pObject = ObjectToolResolveObject(pDocument, plMcpJson::GetString(arguments, "object"), sError);

  if (pObject == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  plObjectAccessorBase* pAccessor = pDocument->GetObjectAccessor();

  if (pAccessor == nullptr)
  {
    out_result.SetError("The document has no object accessor, so its properties cannot be read.");
    return;
  }

  const plRTTI* pType = pObject->GetType();

  if (pType == nullptr)
  {
    out_result.SetError("The object has no type information, so its properties cannot be read.");
    return;
  }

  const bool bIncludeValues = plMcpJson::GetBool(arguments, "includeValues", true);
  const plStringView sSingleProperty = plMcpJson::GetString(arguments, "property");

  // Everything that can fail is resolved before the writer exists. ~plStandardJSONWriter asserts if
  // the stream was not closed properly, so returning early from between BeginObject() and EndObject()
  // kills the editor rather than failing the call - see Status.md.
  const plAbstractProperty* pSingleProp = nullptr;
  plVariant singleIndex;

  if (!sSingleProperty.IsEmpty())
  {
    pSingleProp = pAccessor->FindPropertyByName(pObject, sSingleProperty);

    if (pSingleProp == nullptr)
    {
      plStringBuilder s;
      s.SetFormat("'{}' has no property '{}'. Call without 'property' to list the ones it has.", pType->GetTypeName(), sSingleProperty);
      out_result.SetError(s);
      return;
    }

    singleIndex = ObjectToolGetIndex(arguments, pSingleProp->GetCategory());

    if (singleIndex.IsValid())
    {
      // An out of range index asserts inside the reflection layer, which would also take the editor
      // down, so it is rejected before anything reads through it.
      plStringBuilder sIndexError;
      if (ObjectToolValidateIndex(pAccessor, pObject, pSingleProp, singleIndex, false, sIndexError).Failed())
      {
        out_result.SetError(sIndexError);
        return;
      }
    }
  }

  plMcpJsonWriter writer;
  writer.BeginObject();

  plStringBuilder sObjectGuid;
  plConversionUtils::ToString(pObject->GetGuid(), sObjectGuid);
  writer.AddVariableString("object", sObjectGuid);
  writer.AddVariableString("objectType", pType->GetTypeName());

  if (pSingleProp != nullptr)
  {
    const plAbstractProperty* pProp = pSingleProp;
    const plVariant index = singleIndex;

    if (index.IsValid())
    {
      // One container element, which is the follow up call the container listing invites.
      plVariant value;
      const plStatus res = pAccessor->GetValueByName(pObject, sSingleProperty, value, index);

      if (res.Failed())
      {
        // Reported as a field rather than through SetError(), because the writer is already open.
        // EndAll() closes whatever is still open so the partial result is still valid JSON.
        writer.AddVariableString("property", sSingleProperty);
        writer.AddVariableString("error", res.GetMessageString());
        writer.EndAll();

        out_result.m_sText = writer.GetResult();
        out_result.m_bIsError = true;
        return;
      }

      writer.AddVariableString("property", sSingleProperty);
      writer.BeginVariable("index");
      writer.WriteVariant(index);
      writer.EndVariable();

      // An element that is an object reports its guid, matching how member properties are reported.
      if (value.IsA<plUuid>())
      {
        const plDocumentObject* pChild = pDocument->GetObjectManager()->GetObject(value.Get<plUuid>());

        plStringBuilder sChildGuid;
        plConversionUtils::ToString(value.Get<plUuid>(), sChildGuid);
        writer.AddVariableString("objectGuid", sChildGuid);

        if (pChild != nullptr && pChild->GetType() != nullptr)
          writer.AddVariableString("objectType", pChild->GetType()->GetTypeName());
      }
      else
      {
        writer.BeginVariable("value");
        writer.WriteVariant(value);
        writer.EndVariable();
      }

      writer.EndObject();
      out_result.m_sText = writer.GetResult();
      return;
    }

    writer.BeginArray("properties");
    WritePropertyValue(writer, pAccessor, pObject, pProp, bIncludeValues);
    writer.EndArray();
    writer.EndObject();

    out_result.m_sText = writer.GetResult();
    return;
  }

  writer.BeginArray("properties");

  plUInt32 uiCount = 0;

  // GetAllProperties() includes inherited ones, which is right here even though this tool only reads
  // *direct* properties: 'direct' means 'not recursing into sub objects', not 'declared on this exact
  // type'. An asset's settings routinely come partly from a base class, and a caller asking what it
  // can change on this object does not care which class declared what.
  plDynamicArray<const plAbstractProperty*> allProperties;
  pType->GetAllProperties(allProperties);

  for (const plAbstractProperty* pProp : allProperties)
  {
    if (pProp == nullptr)
      continue;

    // Functions are not values and constants belong to the type rather than the object.
    if (pProp->GetCategory() == plPropertyCategory::Function || pProp->GetCategory() == plPropertyCategory::Constant)
      continue;

    ++uiCount;
    WritePropertyValue(writer, pAccessor, pObject, pProp, bIncludeValues);
  }

  writer.EndArray();
  writer.AddVariableUInt32("count", uiCount);
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpObjectTool::ExecuteTree(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sDocument = plMcpJson::GetString(arguments, "document");

  plDocument* pDocument = ObjectToolFindDocument(sDocument);

  if (pDocument == nullptr)
  {
    plStringBuilder s;
    s.SetFormat("No open document matches '{}'. Open it first with 'document_open'. 'document_list' reports what is open.", sDocument);
    out_result.SetError(s);
    return;
  }

  const plStringView sObject = plMcpJson::GetString(arguments, "object");
  const plDocumentObject* pStart = nullptr;

  if (sObject.IsEmpty())
  {
    // Deliberately not ObjectToolResolveObject(): its empty-argument case means 'the single top level
    // object', which is the very thing a scene does not have. Listing a document always starts at the
    // root, whatever shape the document is.
    pStart = pDocument->GetObjectManager()->GetRootObject();
  }
  else
  {
    plStringBuilder sError;
    pStart = ObjectToolResolveObject(pDocument, sObject, sError);

    if (pStart == nullptr)
    {
      out_result.SetError(sError);
      return;
    }
  }

  if (pStart == nullptr)
  {
    out_result.SetError("The document has no object manager root, so there is nothing to list.");
    return;
  }

  // Clamped rather than rejected: a nonsensical limit from the client should still produce a listing.
  const plUInt32 uiMaxDepth = static_cast<plUInt32>(plMath::Clamp<plInt64>(plMcpJson::GetInt(arguments, "depth", 3), 1, 64));
  plUInt32 uiRemaining = static_cast<plUInt32>(plMath::Clamp<plInt64>(plMcpJson::GetInt(arguments, "maxNodes", 300), 1, 100000));

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("document", pDocument->GetDocumentPath());

  // The root is an implementation detail with no type or properties of its own, so listing from it
  // reports its children as the document's top level objects rather than wrapping them in a node.
  const bool bFromRoot = pStart->GetParent() == nullptr;

  if (bFromRoot)
  {
    writer.AddVariableUInt32("topLevelObjects", pStart->GetChildren().GetCount());
    writer.BeginArray("objects");

    for (const plDocumentObject* pChild : pStart->GetChildren())
    {
      if (uiRemaining == 0)
        break;

      --uiRemaining;
      ObjectToolWriteTreeNode(writer, pChild, 1, uiMaxDepth, uiRemaining);
    }

    writer.EndArray();
  }
  else
  {
    writer.BeginVariable("object");
    ObjectToolWriteTreeNode(writer, pStart, 0, uiMaxDepth, uiRemaining);
    writer.EndVariable();
  }

  if (uiRemaining == 0)
  {
    writer.AddVariableBool("truncated", true);
    writer.AddVariableString("truncatedHint", "The node limit was reached. Raise 'maxNodes', lower 'depth', or start from a specific "
                                              "object to see the rest.");
  }

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpObjectTool::ExecuteModify(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sDocument = plMcpJson::GetString(arguments, "document");

  plDocument* pDocument = ObjectToolFindDocument(sDocument);

  if (pDocument == nullptr)
  {
    plStringBuilder s;
    s.SetFormat("No open document matches '{}'. Open it first with 'document_open'.", sDocument);
    out_result.SetError(s);
    return;
  }

  if (pDocument->IsReadOnly())
  {
    out_result.SetError("The document is read only and cannot be modified.");
    return;
  }

  plStringBuilder sError;
  const plDocumentObject* pObject = ObjectToolResolveObject(pDocument, plMcpJson::GetString(arguments, "object"), sError);

  if (pObject == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  plObjectAccessorBase* pAccessor = pDocument->GetObjectAccessor();

  if (pAccessor == nullptr)
  {
    out_result.SetError("The document has no object accessor, so it cannot be modified.");
    return;
  }

  const plStringView sProperty = plMcpJson::GetString(arguments, "property");

  if (sProperty.IsEmpty())
  {
    out_result.SetError("No 'property' argument given. 'object_properties' lists the properties of an object.");
    return;
  }

  const plAbstractProperty* pProp = pAccessor->FindPropertyByName(pObject, sProperty);

  if (pProp == nullptr)
  {
    plStringBuilder s;
    s.SetFormat("'{}' has no property '{}'. 'object_properties' lists the ones it has.",
      pObject->GetType() != nullptr ? pObject->GetType()->GetTypeName() : plStringView("<unknown>"), sProperty);
    out_result.SetError(s);
    return;
  }

  if (pProp->GetFlags().IsSet(plPropertyFlags::ReadOnly))
  {
    plStringBuilder s;
    s.SetFormat("'{}' is read only.", sProperty);
    out_result.SetError(s);
    return;
  }

  const plStringView sOperation = plMcpJson::GetString(arguments, "operation", "set");
  const plPropertyCategory::Enum category = pProp->GetCategory();
  const plVariant index = ObjectToolGetIndex(arguments, category);

  // The label the user sees in the undo menu. Naming MCP makes it obvious which changes were an
  // agent's rather than their own.
  plStringBuilder sTransactionName;
  sTransactionName.SetFormat("MCP: {} '{}'", sOperation, sProperty);

  plStatus res(PL_SUCCESS);
  plStringBuilder sAddedObjectGuid;

  pAccessor->StartTransaction(sTransactionName);

  if (sOperation == "set" || sOperation == "insert")
  {
    const plVariant* pRawValue = ObjectToolGetRawValue(arguments, "value");

    if (pRawValue == nullptr || !pRawValue->IsValid())
    {
      pAccessor->CancelTransaction();
      plStringBuilder s;
      s.SetFormat("'{}' needs a 'value' argument.", sOperation);
      out_result.SetError(s);
      return;
    }

    plVariant value;
    plStringBuilder sCoerceError;

    if (ObjectToolCoerceValue(*pRawValue, pProp, value, sCoerceError).Failed())
    {
      pAccessor->CancelTransaction();
      out_result.SetError(sCoerceError);
      return;
    }

    if (sOperation == "set")
    {
      // A container without an index would silently do nothing useful, so say what is missing.
      if (category != plPropertyCategory::Member && !index.IsValid())
      {
        pAccessor->CancelTransaction();
        plStringBuilder s;
        s.SetFormat("'{}' is a {}, so 'set' needs an 'index' saying which element to write. Use 'insert' to add a new one.",
          sProperty, ObjectToolCategoryToString(category));
        out_result.SetError(s);
        return;
      }

      plStringBuilder sIndexError;
      if (index.IsValid() && ObjectToolValidateIndex(pAccessor, pObject, pProp, index, false, sIndexError).Failed())
      {
        pAccessor->CancelTransaction();
        out_result.SetError(sIndexError);
        return;
      }

      res = pAccessor->SetValueByName(pObject, sProperty, value, index);
    }
    else
    {
      if (category == plPropertyCategory::Member)
      {
        pAccessor->CancelTransaction();
        plStringBuilder s;
        s.SetFormat("'{}' is a single value, not a container, so it cannot be inserted into. Use 'set'.", sProperty);
        out_result.SetError(s);
        return;
      }

      plVariant insertIndex = index;

      if (!insertIndex.IsValid())
      {
        if (category == plPropertyCategory::Map)
        {
          pAccessor->CancelTransaction();
          plStringBuilder s;
          s.SetFormat("'{}' is a map, so 'insert' needs an 'index' giving the key to store the value under.", sProperty);
          out_result.SetError(s);
          return;
        }

        // Appending is what 'insert without an index' means for a caller, but the accessor takes an
        // invalid index to mean 'no position' and fails with a message about a key called '<Invalid>'.
        // Turning it into 'one past the last element' is what the caller intended.
        plInt32 iCount = 0;
        pAccessor->GetCountByName(pObject, sProperty, iCount).IgnoreResult();
        insertIndex = static_cast<plUInt32>(plMath::Max(iCount, 0));
      }
      else
      {
        plStringBuilder sIndexError;
        if (ObjectToolValidateIndex(pAccessor, pObject, pProp, insertIndex, true, sIndexError).Failed())
        {
          pAccessor->CancelTransaction();
          out_result.SetError(sIndexError);
          return;
        }
      }

      res = pAccessor->InsertValueByName(pObject, sProperty, value, insertIndex);
    }
  }
  else if (sOperation == "remove")
  {
    if (!index.IsValid())
    {
      pAccessor->CancelTransaction();
      out_result.SetError("'remove' needs an 'index' saying which element to remove.");
      return;
    }

    plStringBuilder sIndexError;
    if (ObjectToolValidateIndex(pAccessor, pObject, pProp, index, false, sIndexError).Failed())
    {
      pAccessor->CancelTransaction();
      out_result.SetError(sIndexError);
      return;
    }

    res = pAccessor->RemoveValueByName(pObject, sProperty, index);
  }
  else if (sOperation == "move")
  {
    const plVariant newIndex = [&]()
    {
      const plVariant* pNewIndex = ObjectToolGetRawValue(arguments, "newIndex");

      if (pNewIndex == nullptr || !pNewIndex->IsValid())
        return plVariant();

      if (category == plPropertyCategory::Map)
        return plVariant(pNewIndex->ConvertTo<plString>());

      return pNewIndex->CanConvertTo<plUInt32>() ? plVariant(pNewIndex->ConvertTo<plUInt32>()) : plVariant();
    }();

    if (!index.IsValid() || !newIndex.IsValid())
    {
      pAccessor->CancelTransaction();
      out_result.SetError("'move' needs both 'index' and 'newIndex'.");
      return;
    }

    plStringBuilder sIndexError;

    // The destination may be one past the end, which is how an element is moved to the back.
    if (ObjectToolValidateIndex(pAccessor, pObject, pProp, index, false, sIndexError).Failed() ||
        ObjectToolValidateIndex(pAccessor, pObject, pProp, newIndex, true, sIndexError).Failed())
    {
      pAccessor->CancelTransaction();
      out_result.SetError(sIndexError);
      return;
    }

    res = pAccessor->MoveValueByName(pObject, sProperty, index, newIndex);
  }
  else if (sOperation == "addObject")
  {
    const plStringView sType = plMcpJson::GetString(arguments, "type");

    if (sType.IsEmpty())
    {
      pAccessor->CancelTransaction();
      out_result.SetError("'addObject' needs a 'type' argument naming the type to create.");
      return;
    }

    const plRTTI* pType = plRTTI::FindTypeByName(sType);

    if (pType == nullptr)
    {
      pAccessor->CancelTransaction();
      plStringBuilder s;
      s.SetFormat("'{}' is not a known type. 'rtti_derived_types' lists the types that derive from a given base.", sType);
      out_result.SetError(s);
      return;
    }

    plVariant addIndex = index;

    // Adding into a container without saying where means appending, same as 'insert'.
    if (!addIndex.IsValid() && (category == plPropertyCategory::Array || category == plPropertyCategory::Set))
    {
      plInt32 iCount = 0;
      pAccessor->GetCountByName(pObject, sProperty, iCount).IgnoreResult();
      addIndex = static_cast<plUInt32>(plMath::Max(iCount, 0));
    }
    else if (addIndex.IsValid())
    {
      plStringBuilder sIndexError;
      if (ObjectToolValidateIndex(pAccessor, pObject, pProp, addIndex, true, sIndexError).Failed())
      {
        pAccessor->CancelTransaction();
        out_result.SetError(sIndexError);
        return;
      }
    }

    // Asked of the object manager rather than checked against the property's declared type: a
    // document decides for itself what may go where, and the two do not always agree. The scene's
    // root holds its game objects in a property declared as plReflectedClass, which plGameObject does
    // not derive from - so a type check here rejects the single most common thing anyone would want
    // to do, while the manager, which is what the editor's own UI asks, allows it.
    if (plStatus canAdd = pDocument->GetObjectManager()->CanAdd(pType, pObject, sProperty, addIndex); canAdd.Failed())
    {
      pAccessor->CancelTransaction();
      plStringBuilder s;
      s.SetFormat("'{}' cannot be added to '{}': {}", sType, sProperty, canAdd.m_sMessage);
      out_result.SetError(s);
      return;
    }

    plUuid newObjectGuid;
    res = pAccessor->AddObjectByName(pObject, sProperty, addIndex, pType, newObjectGuid);

    if (res.Succeeded())
      plConversionUtils::ToString(newObjectGuid, sAddedObjectGuid);
  }
  else if (sOperation == "removeObject")
  {
    // GetChildObjectByName() reads through GetValueByName(), so an unchecked index asserts here too.
    plStringBuilder sIndexError;
    if (index.IsValid() && ObjectToolValidateIndex(pAccessor, pObject, pProp, index, false, sIndexError).Failed())
    {
      pAccessor->CancelTransaction();
      out_result.SetError(sIndexError);
      return;
    }

    const plDocumentObject* pChild = pAccessor->GetChildObjectByName(pObject, sProperty, index);

    if (pChild == nullptr)
    {
      pAccessor->CancelTransaction();
      plStringBuilder s;
      s.SetFormat("'{}' holds no object{} to remove.", sProperty, index.IsValid() ? " at that index" : "");
      out_result.SetError(s);
      return;
    }

    res = pAccessor->RemoveObject(pChild);
  }
  else
  {
    pAccessor->CancelTransaction();
    plStringBuilder s;
    s.SetFormat("'{}' is not a valid operation. Use one of: set, insert, remove, move, addObject, removeObject.", sOperation);
    out_result.SetError(s);
    return;
  }

  if (res.Failed())
  {
    // Cancelling rather than finishing leaves no half applied step on the undo stack.
    pAccessor->CancelTransaction();

    plStringBuilder s;
    s.SetFormat("Could not {} '{}': {}", sOperation, sProperty, res.GetMessageString());
    out_result.SetError(s);
    return;
  }

  pAccessor->FinishTransaction();

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("property", sProperty);
  writer.AddVariableString("operation", sOperation);

  if (!sAddedObjectGuid.IsEmpty())
    writer.AddVariableString("addedObject", sAddedObjectGuid);

  writer.AddVariableBool("modified", pDocument->IsModified());

  // Saying this on every successful change is repetitive, but an unsaved change is invisible to the
  // asset system, and an agent that assumes otherwise transforms stale data.
  writer.AddVariableString("note", "The document is not saved yet. Call 'document_save' to write it to disk.");

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

namespace
{
  /// Resolves an object by guid, with no fallback to the top level object.
  ///
  /// The structural tools deliberately do not default to the document's single top level object the
  /// way the property tools do: deleting or moving 'whichever object that turns out to be' is not
  /// something an agent should be able to do by leaving an argument out.
  const plDocumentObject* ObjectToolRequireObject(
    const plDocument* pDocument, plStringView sArgument, plStringView sObjectGuid, plStringBuilder& out_sError)
  {
    if (sObjectGuid.IsEmpty())
    {
      out_sError.SetFormat("The '{}' argument is required and takes an object guid, as reported by 'object_properties'.", sArgument);
      return nullptr;
    }

    if (!plConversionUtils::IsStringUuid(sObjectGuid))
    {
      out_sError.SetFormat("'{}' is not a valid object guid.", sObjectGuid);
      return nullptr;
    }

    const plDocumentObject* pObject = pDocument->GetObjectManager()->GetObject(plConversionUtils::ConvertStringToUuid(sObjectGuid));

    if (pObject == nullptr)
    {
      out_sError.SetFormat("The document has no object with guid '{}'. Object guids come from a previous 'object_properties' call and "
                           "are not stable across editor sessions.",
        sObjectGuid);
    }

    return pObject;
  }

  /// Writes the identity of one object: guid, type and, where the type has one, its name.
  ///
  /// The name is what the user sees in the scene tree, so a selection reported without it is a list
  /// of guids that neither side can talk about.
  void ObjectToolWriteObjectIdentity(plMcpJsonWriter& ref_writer, plObjectAccessorBase* pAccessor, const plDocumentObject* pObject)
  {
    ref_writer.AddVariableUuid("guid", pObject->GetGuid());

    if (const plRTTI* pType = pObject->GetType())
      ref_writer.AddVariableString("type", pType->GetTypeName());

    plVariant name;
    if (pAccessor != nullptr && pAccessor->GetValueByName(pObject, "Name", name).Succeeded() && name.IsA<plString>())
    {
      const plString sName = name.Get<plString>();

      if (!sName.IsEmpty())
        ref_writer.AddVariableString("name", sName);
    }
  }

  /// The document, accessor and command history, or an error saying which of them was missing.
  ///
  /// Every structural tool needs all three, and the three failure messages are worth getting right
  /// once rather than three times.
  bool ObjectToolGetEditContext(const plVariantDictionary& arguments, plDocument*& out_pDocument, plObjectAccessorBase*& out_pAccessor,
    plMcpToolResult& out_result)
  {
    const plStringView sDocument = plMcpJson::GetString(arguments, "document");

    out_pDocument = ObjectToolFindDocument(sDocument);

    if (out_pDocument == nullptr)
    {
      plStringBuilder s;
      s.SetFormat("No open document matches '{}'. Open it first with 'document_open' - documents are never opened implicitly. "
                  "'document_list' reports what is open.",
        sDocument);
      out_result.SetError(s);
      return false;
    }

    out_pAccessor = out_pDocument->GetObjectAccessor();

    if (out_pAccessor == nullptr)
    {
      out_result.SetError("The document has no object accessor, so its objects cannot be changed.");
      return false;
    }

    return true;
  }
} // namespace

void plMcpObjectTool::ExecuteDelete(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plObjectAccessorBase* pAccessor = nullptr;

  if (!ObjectToolGetEditContext(arguments, pDocument, pAccessor, out_result))
    return;

  plStringBuilder sError;
  const plDocumentObject* pObject = ObjectToolRequireObject(pDocument, "object", plMcpJson::GetString(arguments, "object"), sError);

  if (pObject == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  // Captured before the object is gone, so the result can say what was deleted rather than echo the
  // guid the caller already had.
  plStringBuilder sType = pObject->GetType() != nullptr ? pObject->GetType()->GetTypeName() : plStringView("<unknown>");
  const plUInt32 uiChildren = pObject->GetChildren().GetCount();

  pAccessor->StartTransaction("Delete Object");

  const plStatus res = pAccessor->RemoveObject(pObject);

  if (res.Failed())
  {
    pAccessor->CancelTransaction();
    sError.SetFormat("Could not delete the object: {}", res.GetMessageString());
    out_result.SetError(sError);
    return;
  }

  pAccessor->FinishTransaction();

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("deletedType", sType);
  writer.AddVariableUInt32("childrenDeleted", uiChildren);
  writer.AddVariableBool("modified", pDocument->IsModified());
  writer.AddVariableString("note", "The document is not saved yet. Call 'document_save' to write it to disk.");
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpObjectTool::ExecuteReparent(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plObjectAccessorBase* pAccessor = nullptr;

  if (!ObjectToolGetEditContext(arguments, pDocument, pAccessor, out_result))
    return;

  plStringBuilder sError;
  const plDocumentObject* pObject = ObjectToolRequireObject(pDocument, "object", plMcpJson::GetString(arguments, "object"), sError);

  if (pObject == nullptr)
  {
    out_result.SetError(sError);
    return;
  }

  // An omitted 'newParent' means the top level, which plMoveObjectCommand expresses as no parent.
  const plStringView sNewParent = plMcpJson::GetString(arguments, "newParent");
  const plDocumentObject* pNewParent = nullptr;

  if (!sNewParent.IsEmpty())
  {
    pNewParent = ObjectToolRequireObject(pDocument, "newParent", sNewParent, sError);

    if (pNewParent == nullptr)
    {
      out_result.SetError(sError);
      return;
    }

    // Moving an object underneath itself would detach that whole subtree from the document. The
    // command layer rejects it, but not before the transaction is open, and the message it gives is
    // about parent guids rather than about what was asked for.
    for (const plDocumentObject* pWalk = pNewParent; pWalk != nullptr; pWalk = pWalk->GetParent())
    {
      if (pWalk == pObject)
      {
        out_result.SetError("Cannot move an object underneath itself or one of its own children.");
        return;
      }
    }
  }

  // Keeping the object in the property it already sits in is right for a scene, where both ends are
  // 'Children', and is the only sensible default for a document whose properties this tool does not
  // know. Naming a property that the new parent does not have is caught by the command.
  plStringView sProperty = plMcpJson::GetString(arguments, "property");

  if (sProperty.IsEmpty())
    sProperty = pObject->GetParentProperty();

  if (sProperty.IsEmpty())
  {
    out_result.SetError("The object sits in no named property, so 'property' has to say which property of the new parent receives it.");
    return;
  }

  const plDocumentObject* pPropertyOwner = pNewParent != nullptr ? pNewParent : pDocument->GetObjectManager()->GetRootObject();
  const plAbstractProperty* pProp = pAccessor->FindPropertyByName(pPropertyOwner, sProperty);

  if (pProp == nullptr)
  {
    sError.SetFormat("'{}' has no property '{}'. Use 'object_properties' on the new parent to see the ones it has.",
      pPropertyOwner->GetType() != nullptr ? pPropertyOwner->GetType()->GetTypeName() : plStringView("The new parent"), sProperty);
    out_result.SetError(sError);
    return;
  }

  // Appending by default. plMoveObjectCommand treats an index past the end as 'at the end', so an
  // out of range value from a caller that guessed is harmless here.
  plVariant index = plMcpJson::GetInt(arguments, "index", -1) < 0
                      ? plVariant(static_cast<plUInt32>(pAccessor->GetCount(pPropertyOwner, pProp)))
                      : plVariant(static_cast<plUInt32>(plMcpJson::GetInt(arguments, "index", 0)));

  pAccessor->StartTransaction("Reparent Object");

  const plStatus res = pAccessor->MoveObject(pObject, pNewParent, pProp, index);

  if (res.Failed())
  {
    pAccessor->CancelTransaction();
    sError.SetFormat("Could not move the object: {}", res.GetMessageString());
    out_result.SetError(sError);
    return;
  }

  pAccessor->FinishTransaction();

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableUuid("object", pObject->GetGuid());

  if (pNewParent != nullptr)
    writer.AddVariableUuid("newParent", pNewParent->GetGuid());
  else
    writer.AddVariableString("newParent", "<document root>");

  writer.AddVariableString("property", sProperty);
  writer.AddVariableBool("modified", pDocument->IsModified());
  writer.AddVariableString("note", "The document is not saved yet. Call 'document_save' to write it to disk.");
  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpObjectTool::ExecuteSelect(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plDocument* pDocument = nullptr;
  plObjectAccessorBase* pAccessor = nullptr;

  if (!ObjectToolGetEditContext(arguments, pDocument, pAccessor, out_result))
    return;

  plSelectionManager* pSelection = pDocument->GetSelectionManager();

  if (pSelection == nullptr)
  {
    out_result.SetError("The document has no selection manager.");
    return;
  }

  const plVariant* pObjectsArg = nullptr;
  arguments.TryGetValue("objects", pObjectsArg);

  const plVariantArray* pObjectGuids = pObjectsArg != nullptr && pObjectsArg->IsA<plVariantArray>() ? &pObjectsArg->Get<plVariantArray>() : nullptr;

  plStringView sAction = plMcpJson::GetString(arguments, "action");

  if (sAction.IsEmpty())
    sAction = pObjectGuids != nullptr ? plStringView("set") : plStringView("get");

  if (sAction != "get" && sAction != "set" && sAction != "add" && sAction != "remove" && sAction != "clear")
  {
    plStringBuilder s;
    s.SetFormat("'{}' is not a valid action. Use one of: get, set, add, remove, clear.", sAction);
    out_result.SetError(s);
    return;
  }

  // Everything is resolved before anything is changed, so a list with one bad guid in it leaves the
  // selection as it was rather than half applied.
  plDeque<const plDocumentObject*> objects;
  plStringBuilder sError;

  if (pObjectGuids != nullptr)
  {
    for (const plVariant& entry : *pObjectGuids)
    {
      plStringBuilder sGuid;

      if (entry.IsA<plString>())
        sGuid = entry.Get<plString>();
      else if (entry.CanConvertTo<plString>())
        sGuid = entry.ConvertTo<plString>();

      const plDocumentObject* pObject = ObjectToolRequireObject(pDocument, "objects", sGuid, sError);

      if (pObject == nullptr)
      {
        out_result.SetError(sError);
        return;
      }

      objects.PushBack(pObject);
    }
  }

  if ((sAction == "set" || sAction == "add" || sAction == "remove") && objects.IsEmpty())
  {
    plStringBuilder s;
    s.SetFormat("'{}' needs a non-empty 'objects' array. Use 'clear' to select nothing.", sAction);
    out_result.SetError(s);
    return;
  }

  if (sAction == "set")
  {
    pSelection->SetSelection(objects);
  }
  else if (sAction == "add")
  {
    for (const plDocumentObject* pObject : objects)
    {
      pSelection->AddObject(pObject);
    }
  }
  else if (sAction == "remove")
  {
    for (const plDocumentObject* pObject : objects)
    {
      pSelection->RemoveObject(pObject);
    }
  }
  else if (sAction == "clear")
  {
    pSelection->Clear();
  }

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableString("action", sAction);

  writer.BeginArray("selection");
  for (const plDocumentObject* pObject : pSelection->GetSelection())
  {
    writer.BeginObject();
    ObjectToolWriteObjectIdentity(writer, pAccessor, pObject);
    writer.EndObject();
  }
  writer.EndArray();

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpObjectTool::ExecuteUndo(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sDocument = plMcpJson::GetString(arguments, "document");

  plDocument* pDocument = ObjectToolFindDocument(sDocument);

  if (pDocument == nullptr)
  {
    plStringBuilder s;
    s.SetFormat("No open document matches '{}'.", sDocument);
    out_result.SetError(s);
    return;
  }

  plCommandHistory* pHistory = pDocument->GetCommandHistory();

  if (pHistory == nullptr)
  {
    out_result.SetError("The document has no command history.");
    return;
  }

  const plStringView sAction = plMcpJson::GetString(arguments, "action");
  const plUInt32 uiCount = static_cast<plUInt32>(plMath::Max<plInt64>(plMcpJson::GetInt(arguments, "count", 1), 1));

  plUInt32 uiDone = 0;
  plStringBuilder sFailure;

  if (sAction == "undo")
  {
    for (plUInt32 i = 0; i < uiCount && pHistory->CanUndo(); ++i)
    {
      if (pHistory->Undo().Failed())
      {
        sFailure = "Undo failed.";
        break;
      }

      ++uiDone;
    }

    // Distinguishes 'nothing to undo' from 'undo did not work', which call for different responses.
    if (uiDone == 0 && sFailure.IsEmpty())
      sFailure = "There is nothing to undo in this document.";
  }
  else if (sAction == "redo")
  {
    for (plUInt32 i = 0; i < uiCount && pHistory->CanRedo(); ++i)
    {
      if (pHistory->Redo().Failed())
      {
        sFailure = "Redo failed.";
        break;
      }

      ++uiDone;
    }

    if (uiDone == 0 && sFailure.IsEmpty())
      sFailure = "There is nothing to redo in this document.";
  }
  else if (!sAction.IsEmpty())
  {
    plStringBuilder s;
    s.SetFormat("'{}' is not a valid action. Use 'undo' or 'redo', or omit it to only report the stack.", sAction);
    out_result.SetError(s);
    return;
  }

  plMcpJsonWriter writer;
  writer.BeginObject();

  if (!sAction.IsEmpty())
  {
    writer.AddVariableString("action", sAction);
    writer.AddVariableUInt32("stepsPerformed", uiDone);
  }

  writer.AddVariableBool("canUndo", pHistory->CanUndo());
  writer.AddVariableBool("canRedo", pHistory->CanRedo());
  writer.AddVariableBool("modified", pDocument->IsModified());

  if (!sFailure.IsEmpty())
    writer.AddVariableString("error", sFailure);

  writer.EndObject();

  out_result.m_sText = writer.GetResult();

  // Asking to undo when there is nothing to undo is a failed call, not a silent no-op.
  out_result.m_bIsError = !sFailure.IsEmpty();
}
