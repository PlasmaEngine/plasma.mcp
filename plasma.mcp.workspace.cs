// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaMcpWorkspace : WorkspaceRules
{
    public PlasmaMcpWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("EditorPluginPlasmaMcp");
    }
}
