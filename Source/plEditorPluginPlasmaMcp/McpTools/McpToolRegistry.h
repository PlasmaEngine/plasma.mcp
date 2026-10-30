#pragma once

#include <plEditorPluginPlasmaMcp/McpTools/McpTool.h>

#include <Foundation/Containers/Map.h>
#include <Foundation/Containers/Set.h>

/// \brief Owns one instance of every plMcpToolProvider and routes calls to them.
///
/// The registry is filled through reflection, so a provider in another editor plugin needs no
/// registration call - it only has to exist.
class PL_EDITORPLUGINMCP_DLL plMcpToolRegistry
{
public:
  /// \brief Instantiates every provider type that isn't known yet.
  ///
  /// Idempotent, and called again when the server starts and after every plugin load, because editor
  /// plugins may be loaded after this one and would otherwise never be picked up.
  static void UpdateProviders();

  /// \brief Destroys the providers (and their tools) whose type comes from the given plugin binary.
  ///
  /// Called before that plugin is unloaded, while the provider code is still mapped.
  static void RemoveProvidersOfPlugin(plStringView sPluginBinary);

  /// \brief Destroys all providers. Called when the plugin is unloaded.
  static void Clear();

  /// \brief All tools of all providers, in registration order.
  static const plDynamicArray<plMcpToolDesc>& GetTools() { return s_Tools; }

  /// \brief Runs a tool. Fails only if no tool of that name exists - a tool that ran but didn't like
  /// its arguments reports that through out_result instead.
  static plResult Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result);

private:
  static plSet<const plRTTI*> s_KnownTypes;
  static plDynamicArray<plMcpToolProvider*> s_Providers;
  static plDynamicArray<plMcpToolDesc> s_Tools;
  static plMap<plString, plMcpToolProvider*> s_ToolLookup;
};
