#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpTools/ProjectTool.h>

#include <Foundation/Application/SdkVersion.h>
#include <Foundation/Configuration/Plugin.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/EditorApp/Configuration/PackageStore.h>
#include <EditorFramework/EditorApp/Configuration/ProjectPackages.h>
#include <EditorFramework/EditorApp/EditorApp.h>
#include <EditorFramework/Project/ProjectExport.h>
#include <ToolsFoundation/Project/ToolsProject.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpProjectTool, 1, plRTTIDefaultAllocator<plMcpProjectTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

void plMcpProjectTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& out_tools) const
{
  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "project_info";
    desc.m_sDescription = "Returns which project the editor currently has open: its name, the project directory, the configured "
                          "data directories, the asset profiles and the loaded plugins. Call this first - paths returned by other "
                          "tools are relative to these data directories, asset transform states depend on the active profile, and "
                          "the loaded plugins determine which reflected types exist.";
    desc.m_sInputSchema = R"({"type":"object","properties":{}})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "package_list";
    desc.m_sDescription = "Lists the installed plugin packages: version, where each one lives on disk, whether it was built from "
                          "local source, whether its binaries match this engine's ABI, the data directories it mounts, and for "
                          "each plugin it declares whether that plugin is actually loaded. "
                          "This is the tool for anything that behaves as though a package were not installed - a missing type, an "
                          "asset that cannot be found, a plugin that did not load. "
                          "Note in particular 'active': several versions of one package can be installed at once and only the "
                          "newest is loaded, so editing files under an older version's directory has no effect. Reads only.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("id":{"type":"string","description":"Only report packages whose id contains this text, e.g. 'csharp'. Case insensitive."},)"
                          R"("allVersions":{"type":"boolean","description":"If true, report every installed version rather than only the active one. Default false. Use this when a fix to a package appears to have had no effect."})"
                          R"(}})";
  }

  {
    plMcpToolDesc& desc = out_tools.ExpandAndGetRef();
    desc.m_sName = "project_export";
    desc.m_sDescription = "Exports the project as a runnable game into a target directory: the transformed assets, the runtime "
                          "binaries, the data directory configuration and a launch config. Installed packages are flattened into "
                          "one Bin directory, so the exported game has no notion of packages.\n"
                          "This is the non-interactive form of the editor's export. Do NOT reach it through action_execute and "
                          "'Project.ExportProject' - that opens a modal dialog and hangs the editor with nothing left to serve.\n"
                          "IMPORTANT - this takes minutes and blocks the editor for the whole time, longer still with "
                          "'transformFirst'. Use a timeout in the tens of minutes and know the process id from app_info first. "
                          "The target directory is emptied before the export, so never point it at a directory holding anything "
                          "else.";
    desc.m_sInputSchema = R"({"type":"object","properties":{)"
                          R"("targetDirectory":{"type":"string","description":"Absolute path to export into. Its existing contents are deleted."},)"
                          R"("profile":{"type":"string","description":"Name of the asset platform profile to export for. Defaults to the active one, which project_info reports."},)"
                          R"("launchScene":{"type":"string","description":"Part of the path of the scene the game should start with. The first scene whose path contains this text is put first in the launch config."},)"
                          R"("transformFirst":{"type":"boolean","description":"Transform all assets before exporting. Default true, which is what the export dialog does and what makes the export correct rather than merely quick - an export of stale assets ships stale data. Set to false only when a full transform has just been run."})"
                          R"(},"required":["targetDirectory"]})";
  }
}

void plMcpProjectTool::Execute(plStringView sToolName, const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (sToolName == "project_info")
    ExecuteProjectInfo(arguments, out_result);
  else if (sToolName == "package_list")
    ExecutePackageList(arguments, out_result);
  else if (sToolName == "project_export")
    ExecuteProjectExport(arguments, out_result);
}

void plMcpProjectTool::ExecuteProjectInfo(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  plMcpJsonWriter writer;
  writer.BeginObject();

  // Reported explicitly rather than as an error, because "no project open" is a legitimate state that
  // the agent has to be able to observe - it explains why every other tool returns nothing.
  const bool bProjectOpen = plToolsProject::IsProjectOpen();
  writer.AddVariableBool("projectOpen", bProjectOpen);

  if (!bProjectOpen)
  {
    writer.EndObject();
    out_result.m_sText = writer.GetResult();
    return;
  }

  const plToolsProject* pProject = plToolsProject::GetSingleton();

  writer.AddVariableString("projectName", pProject->GetProjectName(false));
  writer.AddVariableString("projectDirectory", pProject->GetProjectDirectory());
  writer.AddVariableString("projectFile", pProject->GetProjectFile());
  writer.AddVariableString("projectDataFolder", pProject->GetProjectDataFolder());

  // The configured directories, not the mounted ones: these are what asset paths are expressed
  // relative to, and the root name is the '>rootname/...' prefix that shows up in those paths.
  writer.BeginArray("dataDirectories");
  for (const auto& dd : plEditorApp::GetSingleton()->GetFileSystemConfig().m_DataDirs)
  {
    writer.BeginObject();
    writer.AddVariableString("path", dd.m_sDataDirSpecialPath);
    writer.AddVariableString("rootName", dd.m_sRootName);
    writer.AddVariableBool("writable", dd.m_bWritable);
    // set for directories the project depends on, which the user cannot remove in the settings dialog
    writer.AddVariableBool("hardCodedDependency", dd.m_bHardCodedDependency);
    writer.EndObject();
  }
  writer.EndArray();

  if (const plAssetCurator* pCurator = plAssetCurator::GetSingleton())
  {
    if (const plPlatformProfile* pActive = pCurator->GetActiveAssetProfile())
    {
      writer.AddVariableString("activeAssetProfile", pActive->GetConfigName());
    }

    writer.BeginArray("assetProfiles");
    for (plUInt32 i = 0; i < pCurator->GetNumAssetProfiles(); ++i)
    {
      if (const plPlatformProfile* pProfile = pCurator->GetAssetProfile(i))
      {
        writer.WriteString(pProfile->GetConfigName());
      }
    }
    writer.EndArray();
  }

  // Which plugins are loaded determines which reflected types exist at all, so this is the companion
  // to the 'plugin' field that rtti_type_info returns for a type.
  plDynamicArray<plPlugin::PluginInfo> pluginInfos;
  plPlugin::GetAllPluginInfos(pluginInfos);

  writer.BeginArray("loadedPlugins");
  for (const plPlugin::PluginInfo& info : pluginInfos)
  {
    writer.BeginObject();
    writer.AddVariableString("name", info.m_sName);

    writer.BeginArray("dependencies");
    for (const plString& sDep : info.m_sDependencies)
    {
      writer.WriteString(sDep);
    }
    writer.EndArray();

    writer.EndObject();
  }
  writer.EndArray();

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}

void plMcpProjectTool::ExecuteProjectExport(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  if (!plToolsProject::IsProjectOpen())
  {
    out_result.SetError("No project is open, so there is nothing to export.");
    return;
  }

  plAssetCurator* pCurator = plAssetCurator::GetSingleton();

  if (pCurator == nullptr)
  {
    out_result.SetError("There is no asset curator, so the assets cannot be prepared for export.");
    return;
  }

  plStringBuilder sTarget = plMcpJson::GetString(arguments, "targetDirectory");
  sTarget.MakeCleanPath();

  if (sTarget.IsEmpty())
  {
    out_result.SetError("The 'targetDirectory' argument is required.");
    return;
  }

  // A relative path would be resolved against the editor's working directory, which is not something
  // a caller can reason about - and this directory gets emptied.
  if (!plPathUtils::IsAbsolutePath(sTarget))
  {
    plStringBuilder s;
    s.SetFormat("'{}' is not an absolute path. The export directory is emptied before use, so it has to be named unambiguously.", sTarget);
    out_result.SetError(s);
    return;
  }

  // Exporting into the project itself would delete the project.
  plStringBuilder sProjectDir = plToolsProject::GetSingleton()->GetProjectDirectory();
  sProjectDir.MakeCleanPath();

  if (sTarget.IsEqual_NoCase(sProjectDir) || sTarget.StartsWith_NoCase(plStringBuilder(sProjectDir, "/")))
  {
    out_result.SetError("The target directory is inside the project directory. The export empties it first, which would delete the "
                        "project. Export somewhere outside it.");
    return;
  }

  const plPlatformProfile* pProfile = pCurator->GetActiveAssetProfile();
  const plStringView sProfileName = plMcpJson::GetString(arguments, "profile");

  if (!sProfileName.IsEmpty())
  {
    pProfile = nullptr;

    for (plUInt32 i = 0; i < pCurator->GetNumAssetProfiles(); ++i)
    {
      const plPlatformProfile* pCandidate = pCurator->GetAssetProfile(i);

      if (pCandidate != nullptr && pCandidate->GetConfigName().IsEqual_NoCase(sProfileName))
      {
        pProfile = pCandidate;
        break;
      }
    }

    if (pProfile == nullptr)
    {
      plStringBuilder s;
      s.SetFormat("No asset profile named '{}'. project_info lists the profiles this project has.", sProfileName);
      out_result.SetError(s);
      return;
    }
  }

  if (pProfile == nullptr)
  {
    out_result.SetError("There is no active asset profile to export for.");
    return;
  }

  const bool bTransformFirst = plMcpJson::GetBool(arguments, "transformFirst", true);

  if (bTransformFirst)
  {
    pCurator->CheckFileSystem();
    pCurator->TransformAllAssets(plTransformFlags::TriggeredManually).IgnoreResult();
  }

  // Collections and the like carry transitive dependencies the change detection does not follow, so
  // they are rebuilt for the export either way. The export dialog does the same, and skips it only
  // when a full transform has just covered it.
  if (!bTransformFirst)
    pCurator->TransformAssetsForSceneExport(pProfile);

  const plStringView sLaunchScene = plMcpJson::GetString(arguments, "launchScene");

  const plResult res = plProjectExport::ExportProject(sTarget, pProfile, plEditorApp::GetSingleton()->GetFileSystemConfig(),
    plArrayPtr<const plString>(), sLaunchScene.IsEmpty() ? nullptr : plStringBuilder(sLaunchScene).GetData());

  plMcpJsonWriter writer;
  writer.BeginObject();
  writer.AddVariableBool("exported", res.Succeeded());
  writer.AddVariableString("targetDirectory", sTarget);
  writer.AddVariableString("profile", pProfile->GetConfigName());
  writer.AddVariableBool("transformedFirst", bTransformFirst);

  if (res.Failed())
  {
    writer.AddVariableString("error", "The export did not complete. The reason is in the editor log - use log_read with severity "
                                      "'error'. A failing asset transform is the usual cause; asset_transform_all names them.");
  }

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
  out_result.m_bIsError = res.Failed();
}

namespace
{
  /// Writes the plugins of one kind, saying for each whether it is loaded right now.
  ///
  /// 'loaded' is only meaningful for the editor plugins: the other two kinds load into the separate
  /// engine process, which this code cannot see. Reporting them as not loaded would be a wrong answer,
  /// so the flag is simply left off them.
  void WritePackagePlugins(plMcpJsonWriter& ref_writer, plStringView sName, const plHybridArray<plString, 2>& plugins,
    const plSet<plString>* pLoaded, plSet<plString>* pOutNotLoaded)
  {
    if (plugins.IsEmpty())
      return;

    ref_writer.BeginArray(sName);
    for (const plString& sPlugin : plugins)
    {
      ref_writer.BeginObject();
      ref_writer.AddVariableString("name", sPlugin);

      if (pLoaded != nullptr)
      {
        const bool bLoaded = pLoaded->Contains(sPlugin);
        ref_writer.AddVariableBool("loaded", bLoaded);

        if (!bLoaded && pOutNotLoaded != nullptr)
          pOutNotLoaded->Insert(sPlugin);
      }

      ref_writer.EndObject();
    }
    ref_writer.EndArray();
  }
} // namespace

void plMcpProjectTool::ExecutePackageList(const plVariantDictionary& arguments, plMcpToolResult& out_result)
{
  const plStringView sIdFilter = plMcpJson::GetString(arguments, "id");
  const bool bAllVersions = plMcpJson::GetBool(arguments, "allVersions", false);

  // A local store rather than the plugin manager dialog's: the dialog owns its instance and is not
  // open, and scanning is what makes the answer current rather than whatever was read at startup.
  plPluginPackageStore store;
  store.Scan();

  plDynamicArray<const plInstalledPackage*> newest;
  store.GetNewestInstalled(newest);

  plSet<plString> loadedPlugins;
  {
    plDynamicArray<plPlugin::PluginInfo> pluginInfos;
    plPlugin::GetAllPluginInfos(pluginInfos);

    for (const plPlugin::PluginInfo& info : pluginInfos)
    {
      loadedPlugins.Insert(info.m_sName);
    }
  }

  plProjectPackages projectPackages;
  if (plToolsProject::IsProjectOpen())
  {
    projectPackages.Load(plToolsProject::GetSingleton()->GetProjectDirectory());
  }

  plMcpJsonWriter writer;
  writer.BeginObject();

  // The ABI tag is what decides whether an installed package can be loaded at all, so it is worth
  // having next to the per-package tags rather than requiring app_info to be called as well.
  writer.AddVariableString("engineVersion", plSdkVersion::GetString());
  writer.AddVariableString("engineAbiTag", plSdkVersion::GetAbiTag());

  // The project's Plugins directory is the only store, so with no project open there is none and
  // nothing but built-ins is reported below.
  if (plToolsProject::IsProjectOpen())
    writer.AddVariableString("projectStore", plPluginPackageStore::GetStoreDir());

  plSet<plString> notLoaded;
  plUInt32 uiReported = 0;

  writer.BeginArray("packages");

  for (const plInstalledPackage& package : store.GetInstalled())
  {
    if (!sIdFilter.IsEmpty() && package.m_sId.FindSubString_NoCase(sIdFilter) == nullptr)
      continue;

    const bool bActive = newest.Contains(&package);

    if (!bActive && !bAllVersions)
      continue;

    ++uiReported;

    writer.BeginObject();
    writer.AddVariableString("id", package.m_sId);
    writer.AddVariableString("version", package.m_sVersion);

    // Several versions of one id can be installed side by side and only the newest is ever loaded.
    // Editing the files of an inactive one looks like it should work and silently does nothing, so
    // this flag is the single most useful field here.
    writer.AddVariableBool("active", bActive);

    if (package.m_bBuiltIn)
    {
      writer.AddVariableString("location", "builtIn");
      writer.AddVariableString("displayName", package.m_sDisplayName);
    }
    else
    {
      writer.AddVariableString("location", "project");
      writer.AddVariableString("rootPath", package.m_sRootPath);
    }

    if (!package.m_sAbiTag.IsEmpty())
    {
      writer.AddVariableString("abiTag", package.m_sAbiTag);
      writer.AddVariableBool("abiCompatible", package.IsAbiCompatible());
    }

    // A package installed from local source is one whose binaries can be rebuilt on this machine,
    // which is the difference between "report this upstream" and "rebuild it".
    if (!package.m_sSourceDir.IsEmpty())
    {
      writer.AddVariableString("sourceDir", package.m_sSourceDir);
      writer.AddVariableBool("canRebuildFromSource", package.CanRebuildFromSource());
    }

    if (const plProjectPackageRef* pRef = projectPackages.Find(package.m_sId))
    {
      writer.AddVariableBool("requestedByProject", true);

      // A project asking for one version while another is installed explains a package that is
      // present but not behaving as the project expects.
      if (!pRef->m_sVersion.IsEmpty() && pRef->m_sVersion != package.m_sVersion)
        writer.AddVariableString("requestedVersion", pRef->m_sVersion);
    }
    else if (!package.m_bBuiltIn)
    {
      writer.AddVariableBool("requestedByProject", false);
    }

    WritePackagePlugins(writer, "editorPlugins", package.m_EditorPlugins, &loadedPlugins, bActive ? &notLoaded : nullptr);

    // No 'loaded' for these two: they live in the engine process, which this plugin cannot inspect.
    WritePackagePlugins(writer, "editorEnginePlugins", package.m_EditorEnginePlugins, nullptr, nullptr);
    WritePackagePlugins(writer, "runtimePlugins", package.m_RuntimePlugins, nullptr, nullptr);

    if (!package.m_PackageDependencies.IsEmpty())
    {
      writer.BeginArray("bundledBinaries");
      for (const plString& sDep : package.m_PackageDependencies)
      {
        writer.WriteString(sDep);
      }
      writer.EndArray();
    }

    // Without a mount a package can only contribute binaries, so a package whose shaders or assets
    // cannot be found is very often one that ships none.
    if (!package.m_Mounts.IsEmpty())
    {
      writer.BeginArray("mounts");
      for (const plInstalledPackage::Mount& mount : package.m_Mounts)
      {
        writer.BeginObject();
        writer.AddVariableString("path", mount.m_sPath);
        writer.AddVariableString("rootName", mount.m_sRootName);
        writer.AddVariableBool("writable", mount.m_bWritable);
        writer.EndObject();
      }
      writer.EndArray();
    }

    writer.EndObject();
  }

  writer.EndArray();

  writer.AddVariableUInt32("packagesReported", uiReported);

  // The editor plugins an active package declares but which did not load. The reason is not recorded
  // anywhere queryable - it is written to the editor log at startup, so log_read is the next step.
  if (!notLoaded.IsEmpty())
  {
    writer.BeginArray("editorPluginsNotLoaded");
    for (const plString& sPlugin : notLoaded)
    {
      writer.WriteString(sPlugin);
    }
    writer.EndArray();

    writer.AddVariableString("editorPluginsNotLoadedHint",
      "These are declared by an installed package but are not loaded in the editor. Check 'abiCompatible' above, then use log_read "
      "with a search for the plugin name - the load failure and its OS error are written to the log at startup.");
  }

  // A package the project asks for but which is not installed at all never appears in the list above,
  // and that absence is the whole answer for a project that behaves as though a plugin were missing.
  plSet<plString> missing;
  for (const plProjectPackageRef& ref : projectPackages.GetPackages())
  {
    if (store.FindAnyVersion(ref.m_sId) == nullptr)
      missing.Insert(ref.m_sId);
  }

  if (!missing.IsEmpty())
  {
    writer.BeginArray("requestedButNotInstalled");
    for (const plString& sId : missing)
    {
      writer.WriteString(sId);
    }
    writer.EndArray();
  }

  writer.EndObject();

  out_result.m_sText = writer.GetResult();
}
