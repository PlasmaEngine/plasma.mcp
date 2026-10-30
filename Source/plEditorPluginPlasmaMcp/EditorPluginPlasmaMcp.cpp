#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpServer.h>
#include <plEditorPluginPlasmaMcp/McpTools/McpToolRegistry.h>

#include <Foundation/Utilities/CommandLineOptions.h>

/// The port to listen on.
///
/// Exists so that several editors can run at the same time, each answering on its own port: with a
/// single fixed port the second editor just fails to bind. That matters for automated testing, where
/// one editor is launched per project and has to be addressable independently of any other.
///
/// The default has to match the URL in the '.mcp.json' that clients are configured with, so changing
/// it means changing those as well. Ports below 1024 are excluded because they need elevation on most
/// systems, and 0 because 'any free port' cannot be put into a client's URL.
static plCommandLineOptionInt s_opt_McpPort("_Mcp", "-mcpport",
  "The port that the editor's MCP server listens on, on 127.0.0.1.\n"
  "Pass a distinct port per editor to run several at the same time.",
  7391, 1024, 0xFFFF);

static plMcpServer* s_pServer = nullptr;

static void ToolsProjectEventHandler(const plToolsProjectEvent& e);

static void PluginEventHandler(const plPluginEvent& e)
{
  // providers can live in plugins that load after this one (or later than project open), and must be
  // gone before their code is unmapped
  if (e.m_EventType == plPluginEvent::AfterLoading)
  {
    plMcpToolRegistry::UpdateProviders();
  }
  else if (e.m_EventType == plPluginEvent::BeforeUnloading)
  {
    plMcpToolRegistry::RemoveProvidersOfPlugin(e.m_sPluginBinary);
  }
}

void OnLoadPlugin()
{
  // s_Events is static, so this works before any project has been opened
  plToolsProject::s_Events.AddEventHandler(ToolsProjectEventHandler);
  plPlugin::Events().AddEventHandler(PluginEventHandler);

  // create the tool providers already now, rather than when the server starts, so that the log tool
  // sees everything that happens during editor startup
  plMcpToolRegistry::UpdateProviders();
}

void OnUnloadPlugin()
{
  plToolsProject::s_Events.RemoveEventHandler(ToolsProjectEventHandler);
  plPlugin::Events().RemoveEventHandler(PluginEventHandler);

  PL_DEFAULT_DELETE(s_pServer);

  plMcpToolRegistry::Clear();
}

PL_PLUGIN_ON_LOADED()
{
  OnLoadPlugin();
}

PL_PLUGIN_ON_UNLOADED()
{
  OnUnloadPlugin();
}

static void ToolsProjectEventHandler(const plToolsProjectEvent& e)
{
  // the server is tied to the open project, because everything it will eventually expose is
  // project specific

  if (e.m_Type == plToolsProjectEvent::Type::ProjectOpened)
  {
    if (s_pServer == nullptr)
    {
      s_pServer = PL_DEFAULT_NEW(plMcpServer);
    }

    const plUInt16 uiPort = static_cast<plUInt16>(s_opt_McpPort.GetOptionValue(plCommandLineOption::LogMode::FirstTimeIfSpecified));

    s_pServer->Start(uiPort).IgnoreResult();
  }

  if (e.m_Type == plToolsProjectEvent::Type::ProjectClosing)
  {
    if (s_pServer != nullptr)
    {
      s_pServer->Stop();
    }
  }
}
