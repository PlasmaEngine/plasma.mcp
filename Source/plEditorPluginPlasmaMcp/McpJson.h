#pragma once

#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpDLL.h>

#include <Foundation/Strings/StringBuilder.h>
#include <Foundation/Types/Variant.h>

/// \brief Reads values out of the arguments of a tool call.
///
/// Every accessor takes a fallback and never fails, because the caller is a language model: arguments
/// go missing, arrive as the wrong type, or are spelled differently than the schema asked for. A tool
/// that wants to reject bad input has to compare against the fallback and say so in its result.
///
/// For producing JSON see ezMcpJsonWriter.
struct PL_EDITORPLUGINMCP_DLL plMcpJson
{
  /// \brief Returns the value of a string member, or sFallback if it is missing or not a string.
  static plStringView GetString(const plVariantDictionary& dict, plStringView sKey, plStringView sFallback = {});

  /// \brief Returns the value of a number member, or iFallback if it is missing or not a number.
  ///
  /// The JSON parser turns every number into a double, and AI clients happily send "10" as a string,
  /// so this accepts anything convertible.
  static plInt64 GetInt(const plVariantDictionary& dict, plStringView sKey, plInt64 iFallback);

  /// \brief Returns the value of a bool member, or bFallback if it is missing or not a bool.
  ///
  /// Also accepts the strings "true"/"false" and numbers, because clients send all three.
  static bool GetBool(const plVariantDictionary& dict, plStringView sKey, bool bFallback);

  /// \brief Returns a nested object member, or nullptr.
  static const plVariantDictionary* GetDict(const plVariantDictionary& dict, plStringView sKey);
};
