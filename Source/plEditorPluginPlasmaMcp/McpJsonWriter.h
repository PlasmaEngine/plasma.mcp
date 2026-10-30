#pragma once

#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpDLL.h>

#include <Foundation/IO/JSONWriter.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Strings/StringBuilder.h>

/// \brief Writes the JSON that tool results and protocol responses are made of.
///
///  - The output goes into an plStringBuilder that the writer owns, so tools never deal with streams.
///  - pl types are written as plain JSON instead of MongoDB-style hex blobs, see WriteVec3().
///  - No whitespace is emitted, because every character is a token the client pays for.
class PL_EDITORPLUGINMCP_DLL plMcpJsonWriter : public plStandardJSONWriter
{
public:
  plMcpJsonWriter();
  ~plMcpJsonWriter();

  /// \brief The JSON written so far.
  ///
  /// Only meaningful once every Begin* has been matched by its End*, because the closing brace is
  /// written by the latter.
  plStringView GetResult();

  /// \brief Splices in text that is already JSON, without escaping or validating it.
  ///
  /// Only for JSON that was authored as such, the tool input schemas in plMcpToolDesc being the reason
  /// this exists. Passing anything else, in particular a plain string or data from outside, produces a
  /// malformed document. Values that need escaping go through WriteString().
  void WriteRawJson(plStringView sJson);

  /// \brief Shorthand for "BeginVariable(sName); WriteRawJson(sJson); EndVariable();"
  void AddVariableRawJson(plStringView sName, plStringView sJson);

  /// The pl types below are written as ordinary JSON objects and arrays, rather than through the base
  /// class' WriteBinaryData(). A model can read and produce {"x":1,"y":2,"z":3}, whereas the
  /// {"$type":"vec3","$binary":"0000803F..."} form of the base class is unusable to it.

  virtual void WriteString(plStringView value) override;

  /// Adds the variant types that the base class asserts on: pointers and embedded objects, which turn
  /// up when reflection data is written generically. The base class' assert is fine for a caller that
  /// knows what it writes, but a tool reading arbitrary reflection data cannot know, and an assert in
  /// a tool call takes the whole editor down.
  virtual void WriteVariant(const plVariant& value) override;

  virtual void WriteVec2(const plVec2& value) override;
  virtual void WriteVec3(const plVec3& value) override;
  virtual void WriteVec4(const plVec4& value) override;
  virtual void WriteVec2I32(const plVec2I32& value) override;
  virtual void WriteVec3I32(const plVec3I32& value) override;
  virtual void WriteVec4I32(const plVec4I32& value) override;

  /// Written as {"x":..,"y":..,"z":..,"w":..}, the same layout that the property system uses.
  virtual void WriteQuat(const plQuat& value) override;

  /// Both colors are written as {"r":..,"g":..,"b":..,"a":..}. Note that plColor is linear and
  /// plColorGammaUB is gamma space with 0-255 components - the JSON does not record which, so a tool
  /// that returns a color should say so in its description.
  virtual void WriteColor(const plColor& value) override;
  virtual void WriteColorGamma(const plColorGammaUB& value) override;

  /// Written row by row, as an array of arrays.
  virtual void WriteMat3(const plMat3& value) override;
  virtual void WriteMat4(const plMat4& value) override;

  /// Written as the usual hyphenated string form, which is how uuids appear everywhere else in the
  /// editor and therefore what an agent will have seen before.
  virtual void WriteUuid(const plUuid& value) override;

  /// Written as degrees, because a model reasons about angles in degrees far more reliably than in
  /// radians, and the editor UI shows degrees as well.
  virtual void WriteAngle(plAngle value) override;

  /// Written as a hex string, which doubles its size and is of little use to a model. Prefer to
  /// return a path or a summary instead of raw binary data.
  virtual void WriteDataBuffer(const plDataBuffer& value) override;

private:
  void WriteFloatArray(const float* pValues, plUInt32 uiCount);

  /// The written bytes are collected here and only turned into a string by GetResult(), because the
  /// writer produces a byte stream and has no notion of a terminating zero.
  plContiguousMemoryStreamStorage m_Storage;
  plMemoryStreamWriter m_Writer;
  plStringBuilder m_sResult;
};
