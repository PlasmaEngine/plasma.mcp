#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>

#include <plEditorPluginPlasmaMcp/McpJson.h>

plStringView plMcpJson::GetString(const plVariantDictionary& dict, plStringView sKey, plStringView sFallback)
{
  const plVariant* pValue = nullptr;

  if (dict.TryGetValue(sKey, pValue) && pValue->IsA<plString>())
  {
    return pValue->Get<plString>().GetView();
  }

  return sFallback;
}

plInt64 plMcpJson::GetInt(const plVariantDictionary& dict, plStringView sKey, plInt64 iFallback)
{
  const plVariant* pValue = nullptr;

  if (!dict.TryGetValue(sKey, pValue) || !pValue->IsValid())
    return iFallback;

  if (pValue->IsNumber())
    return static_cast<plInt64>(pValue->ConvertTo<double>());

  if (pValue->IsA<plString>())
  {
    plInt64 iResult = 0;
    if (plConversionUtils::StringToInt64(pValue->Get<plString>(), iResult).Succeeded())
      return iResult;
  }

  return iFallback;
}

bool plMcpJson::GetBool(const plVariantDictionary& dict, plStringView sKey, bool bFallback)
{
  const plVariant* pValue = nullptr;

  if (!dict.TryGetValue(sKey, pValue) || !pValue->IsValid())
    return bFallback;

  if (pValue->IsA<bool>())
    return pValue->Get<bool>();

  if (pValue->IsNumber())
    return pValue->ConvertTo<double>() != 0.0;

  if (pValue->IsA<plString>())
  {
    bool bResult = false;
    if (plConversionUtils::StringToBool(pValue->Get<plString>(), bResult) == PL_SUCCESS)
      return bResult;
  }

  return bFallback;
}

const plVariantDictionary* plMcpJson::GetDict(const plVariantDictionary& dict, plStringView sKey)
{
  const plVariant* pValue = nullptr;

  if (dict.TryGetValue(sKey, pValue) && pValue->IsA<plVariantDictionary>())
  {
    return &pValue->Get<plVariantDictionary>();
  }

  return nullptr;
}
