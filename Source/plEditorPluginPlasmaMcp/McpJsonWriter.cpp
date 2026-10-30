#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>

#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Types/Uuid.h>

namespace
{
  /// A control character that JSON has no named escape for. The base class passes these through
  /// unchanged, which produces a document that no parser accepts.
  bool IsUnescapableControlChar(plUInt8 uiByte)
  {
    return uiByte < 0x20 && uiByte != '\b' && uiByte != '\f' && uiByte != '\n' && uiByte != '\r' && uiByte != '\t';
  }
} // namespace

plMcpJsonWriter::plMcpJsonWriter()
  : m_Writer(&m_Storage)
{
  SetOutputStream(&m_Writer);

  // every character here is a token that the client pays for, and nothing reads this by eye
  SetWhitespaceMode(plJSONWriter::WhitespaceMode::None);
  SetArrayMode(plJSONWriter::ArrayMode::InOneLine);
}

plMcpJsonWriter::~plMcpJsonWriter() = default;

plStringView plMcpJsonWriter::GetResult()
{
  const plArrayPtr<const plUInt8> bytes = m_Storage.GetContiguousMemoryRange(0);

  m_sResult = plStringView(reinterpret_cast<const char*>(bytes.GetPtr()), bytes.GetCount());

  return m_sResult.GetView();
}

void plMcpJsonWriter::WriteRawJson(plStringView sJson)
{
  // places the separating comma and marks the value as written, exactly as for any other value
  CommaWriter cw(this);

  // written to the stream directly, because the text must not be escaped or quoted
  m_Writer.WriteBytes(sJson.GetStartPointer(), sJson.GetElementCount()).IgnoreResult();
}

void plMcpJsonWriter::AddVariableRawJson(plStringView sName, plStringView sJson)
{
  BeginVariable(sName);
  WriteRawJson(sJson);
  EndVariable();
}

void plMcpJsonWriter::WriteString(plStringView value)
{
  // The base class handles quotes, backslashes and the named escapes, so the string only has to be
  // touched for the control characters it would let through. Editor log text contains those often
  // enough to matter, and the common case must not pay for a copy.
  bool bNeedsFixup = false;

  for (const char* szCur = value.GetStartPointer(); szCur < value.GetEndPointer(); ++szCur)
  {
    if (IsUnescapableControlChar(static_cast<plUInt8>(*szCur)))
    {
      bNeedsFixup = true;
      break;
    }
  }

  if (!bNeedsFixup)
  {
    plStandardJSONWriter::WriteString(value);
    return;
  }

  plStringBuilder sSanitized;

  for (const char* szCur = value.GetStartPointer(); szCur < value.GetEndPointer(); ++szCur)
  {
    if (IsUnescapableControlChar(static_cast<plUInt8>(*szCur)))
    {
      // replaced rather than escaped: writing \uXXXX here is not possible, because the base class
      // would escape the backslash again
      sSanitized.Append(" ");
    }
    else
    {
      sSanitized.Append(plStringView(szCur, szCur + 1));
    }
  }

  plStandardJSONWriter::WriteString(sSanitized);
}

void plMcpJsonWriter::WriteVariant(const plVariant& value)
{
  switch (value.GetType())
  {
    case plVariant::Type::TypedPointer:
    {
      // Written as a description of the target rather than as its contents: the pointer may be null,
      // may point at a type with cycles, and the model can look the type up with the rtti tools anyway.
      const plTypedPointer ptr = value.Get<plTypedPointer>();

      const plRTTI* pType = ptr.m_pType;

      // The variant carries the *declared* pointer type, which for a reflected container is the base
      // class - an array of plPropertyAttribute* reports 'plPropertyAttribute' for every element,
      // losing which attribute it actually is. When the target derives from plReflectedClass it can
      // say so itself, and that is the name worth reporting.
      if (ptr.m_pObject != nullptr && pType != nullptr && pType->IsDerivedFrom<plReflectedClass>())
      {
        pType = static_cast<const plReflectedClass*>(ptr.m_pObject)->GetDynamicRTTI();
      }

      BeginObject();
      AddVariableString("$type", pType != nullptr ? pType->GetTypeName() : plStringView());

      if (ptr.m_pObject == nullptr)
      {
        AddVariableBool("$null", true);
      }

      EndObject();
      return;
    }

    case plVariant::Type::TypedObject:
    {
      // Only the type is written. Serialising the members would need the property system and could
      // recurse without bound, which is not what a JSON writer should be doing.
      const plRTTI* pType = value.GetReflectedType();

      BeginObject();
      AddVariableString("$type", pType != nullptr ? pType->GetTypeName() : plStringView());
      EndObject();
      return;
    }

    case plVariant::Type::Transform:
    {
      // plStandardJSONWriter has no case for this and asserts. Skeleton joints expose their rest pose
      // as an plTransform, so without this, reading one takes the whole editor down.
      const plTransform t = value.Get<plTransform>();

      BeginObject();
      BeginObject("position");
      AddVariableFloat("x", t.m_vPosition.x);
      AddVariableFloat("y", t.m_vPosition.y);
      AddVariableFloat("z", t.m_vPosition.z);
      EndObject();
      BeginObject("rotation");
      AddVariableFloat("x", t.m_qRotation.x);
      AddVariableFloat("y", t.m_qRotation.y);
      AddVariableFloat("z", t.m_qRotation.z);
      AddVariableFloat("w", t.m_qRotation.w);
      EndObject();
      BeginObject("scale");
      AddVariableFloat("x", t.m_vScale.x);
      AddVariableFloat("y", t.m_vScale.y);
      AddVariableFloat("z", t.m_vScale.z);
      EndObject();
      EndObject();
      return;
    }

    // Everything plStandardJSONWriter::WriteVariant actually has a case for. Listed explicitly because
    // its fallthrough is PL_REPORT_FAILURE, so handing it a type it does not know turns reading an
    // ordinary property into an editor crash - and an agent reading properties must never be able to
    // do that. Anything not listed is described instead; give it a real case above if it matters.
    case plVariant::Type::Invalid:
    case plVariant::Type::Bool:
    case plVariant::Type::Int8:
    case plVariant::Type::UInt8:
    case plVariant::Type::Int16:
    case plVariant::Type::UInt16:
    case plVariant::Type::Int32:
    case plVariant::Type::UInt32:
    case plVariant::Type::Int64:
    case plVariant::Type::UInt64:
    case plVariant::Type::Float:
    case plVariant::Type::Double:
    case plVariant::Type::Color:
    case plVariant::Type::ColorGamma:
    case plVariant::Type::Vector2:
    case plVariant::Type::Vector3:
    case plVariant::Type::Vector4:
    case plVariant::Type::Vector2I:
    case plVariant::Type::Vector3I:
    case plVariant::Type::Vector4I:
    case plVariant::Type::Vector2U:
    case plVariant::Type::Vector3U:
    case plVariant::Type::Vector4U:
    case plVariant::Type::Quaternion:
    case plVariant::Type::Matrix3:
    case plVariant::Type::Matrix4:
    case plVariant::Type::String:
    case plVariant::Type::StringView:
    case plVariant::Type::Time:
    case plVariant::Type::Uuid:
    case plVariant::Type::Angle:
    case plVariant::Type::DataBuffer:
    case plVariant::Type::HashedString:
    case plVariant::Type::TempHashedString:
    case plVariant::Type::VariantArray:
    case plVariant::Type::VariantDictionary:
      plStandardJSONWriter::WriteVariant(value);
      return;

    default:
    {
      plStringBuilder s;
      s.SetFormat("<unsupported variant type {}>", static_cast<int>(value.GetType()));
      WriteString(s);
      return;
    }
  }
}

void plMcpJsonWriter::WriteFloatArray(const float* pValues, plUInt32 uiCount)
{
  constexpr plStringView szComponents[] = {"x", "y", "z", "w"};

  PL_ASSERT_DEBUG(uiCount <= PL_ARRAY_SIZE(szComponents), "Only up to 4 components are supported.");

  BeginObject();

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    AddVariableFloat(szComponents[i], pValues[i]);
  }

  EndObject();
}

void plMcpJsonWriter::WriteVec2(const plVec2& value)
{
  WriteFloatArray(&value.x, 2);
}

void plMcpJsonWriter::WriteVec3(const plVec3& value)
{
  WriteFloatArray(&value.x, 3);
}

void plMcpJsonWriter::WriteVec4(const plVec4& value)
{
  WriteFloatArray(&value.x, 4);
}

void plMcpJsonWriter::WriteVec2I32(const plVec2I32& value)
{
  BeginObject();
  AddVariableInt32("x", value.x);
  AddVariableInt32("y", value.y);
  EndObject();
}

void plMcpJsonWriter::WriteVec3I32(const plVec3I32& value)
{
  BeginObject();
  AddVariableInt32("x", value.x);
  AddVariableInt32("y", value.y);
  AddVariableInt32("z", value.z);
  EndObject();
}

void plMcpJsonWriter::WriteVec4I32(const plVec4I32& value)
{
  BeginObject();
  AddVariableInt32("x", value.x);
  AddVariableInt32("y", value.y);
  AddVariableInt32("z", value.z);
  AddVariableInt32("w", value.w);
  EndObject();
}

void plMcpJsonWriter::WriteQuat(const plQuat& value)
{
  WriteFloatArray(&value.x, 4);
}

void plMcpJsonWriter::WriteColor(const plColor& value)
{
  BeginObject();
  AddVariableFloat("r", value.r);
  AddVariableFloat("g", value.g);
  AddVariableFloat("b", value.b);
  AddVariableFloat("a", value.a);
  EndObject();
}

void plMcpJsonWriter::WriteColorGamma(const plColorGammaUB& value)
{
  BeginObject();
  AddVariableUInt32("r", value.r);
  AddVariableUInt32("g", value.g);
  AddVariableUInt32("b", value.b);
  AddVariableUInt32("a", value.a);
  EndObject();
}

void plMcpJsonWriter::WriteMat3(const plMat3& value)
{
  float f[9];
  value.GetAsArray(f, plMatrixLayout::RowMajor);

  BeginArray();

  for (plUInt32 uiRow = 0; uiRow < 3; ++uiRow)
  {
    BeginArray();

    for (plUInt32 uiColumn = 0; uiColumn < 3; ++uiColumn)
    {
      WriteFloat(f[uiRow * 3 + uiColumn]);
    }

    EndArray();
  }

  EndArray();
}

void plMcpJsonWriter::WriteMat4(const plMat4& value)
{
  float f[16];
  value.GetAsArray(f, plMatrixLayout::RowMajor);

  BeginArray();

  for (plUInt32 uiRow = 0; uiRow < 4; ++uiRow)
  {
    BeginArray();

    for (plUInt32 uiColumn = 0; uiColumn < 4; ++uiColumn)
    {
      WriteFloat(f[uiRow * 4 + uiColumn]);
    }

    EndArray();
  }

  EndArray();
}

void plMcpJsonWriter::WriteUuid(const plUuid& value)
{
  plStringBuilder sTmp;
  plConversionUtils::ToString(value, sTmp);

  WriteString(sTmp);
}

void plMcpJsonWriter::WriteAngle(plAngle value)
{
  WriteFloat(value.GetDegree());
}

void plMcpJsonWriter::WriteDataBuffer(const plDataBuffer& value)
{
  // hex, because Foundation has no base64 encoder. This is twice the size of the raw data and of no
  // use to a model - a tool that finds itself calling this should probably return something else,
  // e.g. a path to the data or a summary of it.
  plStringBuilder sHex;

  for (plUInt8 uiByte : value)
  {
    sHex.AppendFormat("{}", plArgU(uiByte, 2, true, 16));
  }

  WriteString(sHex);
}
