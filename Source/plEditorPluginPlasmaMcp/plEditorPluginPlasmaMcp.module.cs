using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginPlasmaMcpModule : ModuleRules
{
    public EditorPluginPlasmaMcpModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/plEditorPluginPlasmaMcp",
            "EditorPluginPlasmaMcpPCH.h", "BUILDSYSTEM_BUILDING_EDITORPLUGINPLASMAMCP_LIB");

        // Editor-side headers and import libraries: EditorFramework, ToolsFoundation and the rest. A
        // runtime module must not take this - it would drag the editor into a shipped game.
        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);

        // the server's poll timer, and the recycle bin of document_delete
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");

        // the server's sockets
        ConfigureForPlatform(TargetPlatform.Windows, () =>
        {
            PrivateLibraries.Add("Ws2_32.lib");
        });
    }
}
