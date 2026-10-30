#pragma once

#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpDLL.h>

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Reflection/Reflection.h>
#include <Foundation/Strings/StringBuilder.h>
#include <Foundation/Types/Variant.h>

class plDocument;

/// \brief Describes one callable function, as it appears in the MCP client's tool list.
struct PL_EDITORPLUGINMCP_DLL plMcpToolDesc
{
  /// The name the client calls. Must be unique across all providers. Convention is lower snake_case,
  /// prefixed with the area, e.g. 'log_write'.
  plString m_sName;

  /// What the tool does. This is the only thing an AI agent has to go by when deciding whether to call
  /// it, so be specific about what it returns and what it modifies.
  plString m_sDescription;

  /// The JSON schema of the arguments, as a raw JSON object. Empty means 'no arguments'.
  /// E.g. R"({"type":"object","properties":{"count":{"type":"number"}}})"
  plString m_sInputSchema;
};

/// \brief The result of one tool call.
struct PL_EDITORPLUGINMCP_DLL plMcpToolResult
{
  /// The text handed back to the client. For structured data just write JSON in here - MCP has no
  /// separate typed result, everything is text content.
  plStringBuilder m_sText;

  /// If true, the client is told the call failed. The text should then say why.
  bool m_bIsError = false;

  void SetError(plStringView sMessage)
  {
    m_sText = sMessage;
    m_bIsError = true;
  }
};

/// \brief Base class for everything that the MCP server exposes to AI agents.
///
/// One provider offers one or more tools, which is why they are grouped: 'log_write' and 'log_read'
/// share a ring buffer, so they belong to the same object.
///
/// Providers are found through reflection, so **any editor plugin can add its own** simply by deriving
/// from this class - there is no list to register with. The plugin only has to link against
/// EditorPluginMcp. See plMcpLogTool for the smallest complete example.
///
/// Everything happens on the main thread, so implementations may touch the editor directly.
class PL_EDITORPLUGINMCP_DLL plMcpToolProvider : public plReflectedClass
{
  PL_ADD_DYNAMIC_REFLECTION(plMcpToolProvider, plReflectedClass);

public:
  /// \brief Called once, right after the provider was instantiated. Use it to hook into the editor.
  virtual void OnActivate() {}

  /// \brief Called once, before the provider is destroyed.
  virtual void OnDeactivate() {}

  /// \brief Appends the description of every tool that this provider implements.
  virtual void GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const = 0;

  /// \brief Executes one of the tools returned by GetSupportedTools().
  ///
  /// \param sToolName The tool to run. A provider with a single tool may ignore this.
  /// \param arguments The 'arguments' object of the call. Missing and mistyped values are normal -
  ///        the client is an AI and will get this wrong. Validate and report through out_result.
  virtual void Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result) = 0;

  /// \brief Resolves a guid or a path to an already open document, or nullptr.
  ///
  /// A path may be relative and is matched against the tail of the open documents' paths, because the
  /// paths the asset tools report are data directory relative while a document knows its absolute one.
  ///
  /// Documents are never opened implicitly by this: an agent that edited a document it did not open
  /// would leave it open invisibly, and the undo stack lives on the document, so opening and closing
  /// one per call would make undo meaningless.
  static plDocument* FindOpenDocument(plStringView sIdentifier);
};
