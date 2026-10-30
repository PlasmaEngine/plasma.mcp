#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/IPC/EngineProcessConnection.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/JSONReader.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/Strings/PathUtils.h>
#include <Foundation/Strings/UnicodeUtils.h>
#include <RendererCore/Debug/GpuDump/GpuDumpData.h>
#include <cmath>
#include <plEditorPluginPlasmaMcp/EditorPluginPlasmaMcpPCH.h>
#include <plEditorPluginPlasmaMcp/McpJson.h>
#include <plEditorPluginPlasmaMcp/McpJsonWriter.h>
#include <plEditorPluginPlasmaMcp/McpServer.h>
#include <plEditorPluginPlasmaMcp/McpTools/GpuDumpTool.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plMcpGpuDumpTool, 1, plRTTIDefaultAllocator<plMcpGpuDumpTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;

namespace
{
  /// The completed dumps the viewer may serve: folder name to absolute path.
  plMap<plString, plString> s_Roots;

  plString Arg(const plVariantDictionary& args, const char* key)
  {
    return plMcpJson::GetString(args, key);
  }

  int Int(const plVariantDictionary& args, const char* key, int fallback)
  {
    const plVariant* value = nullptr;
    return args.TryGetValue(key, value) ? value->ConvertTo<int>() : fallback;
  }

  // dump.json as parsed: objects are plVariantDictionary, arrays plVariantArray and every number a double

  const plVariantDictionary* JsonObject(const plVariant& value)
  {
    return value.IsA<plVariantDictionary>() ? &value.Get<plVariantDictionary>() : nullptr;
  }

  const plVariantDictionary* JsonObject(const plVariantDictionary& object, const char* key)
  {
    const plVariant* value = nullptr;
    return object.TryGetValue(key, value) ? JsonObject(*value) : nullptr;
  }

  const plVariantArray* JsonArray(const plVariantDictionary& object, const char* key)
  {
    const plVariant* value = nullptr;
    return object.TryGetValue(key, value) && value->IsA<plVariantArray>() ? &value->Get<plVariantArray>() : nullptr;
  }

  int JsonInt(const plVariantDictionary& object, const char* key)
  {
    const plVariant* value = nullptr;
    return object.TryGetValue(key, value) && value->IsNumber() ? value->ConvertTo<int>() : 0;
  }

  plStringView JsonString(const plVariantDictionary& object, const char* key)
  {
    const plVariant* value = nullptr;
    return object.TryGetValue(key, value) && value->IsA<plString>() ? value->Get<plString>().GetView() : plStringView();
  }

  void Result(plMcpToolResult& result, const plVariant& value)
  {
    plMcpJsonWriter writer;
    writer.WriteVariant(value);
    result.m_sText = writer.GetResult();
  }

  /// The absolute, clean path of an existing file or folder in its stored casing; empty when it does not exist.
  plString Canonical(plStringView sPath)
  {
    if (sPath.IsEmpty())
      return {};

    plStringBuilder sAbsolute = plOSFile::MakePathAbsoluteWithCWD(sPath);
    sAbsolute.MakeCleanPath();

    if (!plOSFile::ExistsFile(sAbsolute) && !plOSFile::ExistsDirectory(sAbsolute))
      return {};

    plStringBuilder sCasing;
    if (plOSFile::GetFileCasing(sAbsolute, sCasing).Failed())
      return sAbsolute;

    sCasing.MakeCleanPath();
    return sCasing;
  }

  /// An existing file inside the dump folder; empty for anything else, including paths that leave the folder.
  plString SafeFile(plStringView sRoot, plStringView sRelative)
  {
    if (sRoot.IsEmpty() || sRelative.IsEmpty() || plPathUtils::IsAbsolutePath(sRelative))
      return {};

    plStringBuilder sPath = sRoot;
    sPath.AppendPath(sRelative);

    const plString sAbsolute = Canonical(sPath);

    plStringBuilder sPrefix = sRoot;
    sPrefix.Append("/");

    if (sAbsolute.IsEmpty() || !sAbsolute.StartsWith_NoCase(sPrefix))
      return {};

    return sAbsolute;
  }

  /// Reads a file's start, at most uiMaxBytes, cut back to whole UTF-8 characters.
  plResult ReadText(plStringView sFile, plUInt32 uiMaxBytes, plStringBuilder& out_sText)
  {
    plOSFile file;
    PL_SUCCEED_OR_RETURN(file.Open(sFile, plFileOpenMode::Read));

    plDynamicArray<char> text;
    text.SetCountUninitialized(static_cast<plUInt32>(plMath::Min<plUInt64>(file.GetFileSize(), uiMaxBytes)));
    text.SetCountUninitialized(static_cast<plUInt32>(file.Read(text.GetData(), text.GetCount())));

    // a cut through a multi-byte character drops it
    while (!text.IsEmpty() && !plUnicodeUtils::IsValidUtf8(text.GetData(), text.GetData() + text.GetCount()))
      text.PopBack();

    out_sText.Clear();
    if (!text.IsEmpty())
      out_sText.SetSubString_FromTo(text.GetData(), text.GetData() + text.GetCount());

    return PL_SUCCESS;
  }

  /// The parsed dump.json of a dump; fails when it is missing, too large or not a JSON object.
  plResult ReadDump(plStringView sRoot, plJSONReader& out_json)
  {
    plOSFile file;
    if (file.Open(SafeFile(sRoot, "dump.json"), plFileOpenMode::Read).Failed() || file.GetFileSize() > 128 * 1024 * 1024)
      return PL_FAILURE;

    plDynamicArray<plUInt8> content;
    file.ReadAll(content);

    plRawMemoryStreamReader reader(content.GetData(), content.GetCount());
    PL_SUCCEED_OR_RETURN(out_json.Parse(reader));

    return out_json.GetTopLevelElementType() == plJSONReader::ElementType::Dictionary ? PL_SUCCESS : PL_FAILURE;
  }

  plString Register(plStringView sRoot)
  {
    const plString sCanonical = Canonical(sRoot);
    if (sCanonical.EndsWith_NoCase(".partial"))
      return {};

    plStringBuilder sDumpFile = sCanonical;
    sDumpFile.AppendPath("dump.json");

    if (!sCanonical.IsEmpty() && plOSFile::ExistsFile(sDumpFile))
      s_Roots[plPathUtils::GetFileNameAndExtension(sCanonical)] = sCanonical;

    return sCanonical;
  }

  plString PercentEncode(plStringView sText)
  {
    plStringBuilder sEncoded;

    for (const char* szChar = sText.GetStartPointer(); szChar < sText.GetEndPointer(); ++szChar)
    {
      const plUInt8 uiChar = static_cast<plUInt8>(*szChar);
      const bool bUnreserved = (uiChar >= 'a' && uiChar <= 'z') || (uiChar >= 'A' && uiChar <= 'Z') || (uiChar >= '0' && uiChar <= '9') || uiChar == '-' || uiChar == '.' || uiChar == '_' || uiChar == '~';

      if (bUnreserved)
        sEncoded.Append(static_cast<plUInt32>(uiChar));
      else
        sEncoded.AppendFormat("%{}", plArgU(uiChar, 2, true, 16, true));
    }

    return sEncoded;
  }

  /// The decoded text; empty when it is not UTF-8.
  plString PercentDecode(plStringView sText)
  {
    auto hexValue = [](char c) -> plInt32
    {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };

    plDynamicArray<char> decoded;

    for (const char* szChar = sText.GetStartPointer(); szChar < sText.GetEndPointer(); ++szChar)
    {
      if (*szChar == '%' && szChar + 2 < sText.GetEndPointer() && hexValue(szChar[1]) >= 0 && hexValue(szChar[2]) >= 0)
      {
        decoded.PushBack(static_cast<char>(hexValue(szChar[1]) * 16 + hexValue(szChar[2])));
        szChar += 2;
        continue;
      }

      decoded.PushBack(*szChar);
    }

    if (decoded.IsEmpty() || !plUnicodeUtils::IsValidUtf8(decoded.GetData(), decoded.GetData() + decoded.GetCount()))
      return {};

    plStringBuilder sDecoded;
    sDecoded.SetSubString_FromTo(decoded.GetData(), decoded.GetData() + decoded.GetCount());
    return sDecoded;
  }

  /// The content type the viewer's files are served with; empty for files it does not serve.
  plStringView GetContentType(plStringView sExtension)
  {
    if (sExtension.IsEqual_NoCase("html"))
      return "text/html; charset=utf-8";
    if (sExtension.IsEqual_NoCase("js"))
      return "text/javascript; charset=utf-8";
    if (sExtension.IsEqual_NoCase("json"))
      return "application/json";
    if (sExtension.IsEqual_NoCase("png"))
      return "image/png";
    if (sExtension.IsEqual_NoCase("bin") || sExtension.IsEqual_NoCase("zst"))
      return "application/octet-stream";
    if (sExtension.IsEqual_NoCase("md") || sExtension.IsEqual_NoCase("txt"))
      return "text/plain; charset=utf-8";
    return {};
  }
} // namespace

plMcpGpuDumpTool::~plMcpGpuDumpTool()
{
  // the known dumps go with the tool, before the plugin and its allocations are gone
  s_Roots.Clear();
}

void plMcpGpuDumpTool::GetSupportedTools(plDynamicArray<plMcpToolDesc>& tools) const
{
  auto add = [&](const char* name, const char* description, const char* schema)
  { auto& t = tools.ExpandAndGetRef(); t.m_sName = name; t.m_sDescription = description; t.m_sInputSchema = schema; };
  add("engine_gpu_dump", "Capture a development engine frame, wait for writing, return folder, bytes, anomalies, digest and viewer URL. Rendering continues during timing and resumes after capture; this can take minutes.", R"({"type":"object","properties":{"root":{"type":"string"},"view":{"type":"string"},"timingFrames":{"type":"integer","minimum":0,"maximum":256},"compare":{"type":"string"},"timeoutSeconds":{"type":"integer","minimum":1,"maximum":600}}})");
  add("gpu_dump_list", "List complete local GPU dumps, excluding partial captures. Optional directory overrides the default appdata/GpuDumps folder.", R"({"type":"object","properties":{"directory":{"type":"string"}}})");
  add("gpu_dump_pass", "Read one pass with its command stream, bindings and constants from a completed dump.", R"({"type":"object","required":["path","pass"],"properties":{"path":{"type":"string"},"pass":{"type":"integer","minimum":0}}})");
  add("gpu_dump_stats", "Get a resource version's statistics, histograms, raw and preview file references.", R"({"type":"object","required":["path","resource"],"properties":{"path":{"type":"string"},"resource":{"type":"integer","minimum":0},"version":{"type":"integer","minimum":0}}})");
  add("gpu_dump_texels", "Decode an exact rectangle from row-tight little-endian raw texture data. At most 4096 texels. NaN and infinities are returned as strings, never JSON null.", R"({"type":"object","required":["path","resource"],"properties":{"path":{"type":"string"},"resource":{"type":"integer","minimum":0},"version":{"type":"integer","minimum":0},"mip":{"type":"integer","minimum":0},"slice":{"type":"integer","minimum":0},"z":{"type":"integer","minimum":0},"x":{"type":"integer","minimum":0},"y":{"type":"integer","minimum":0},"width":{"type":"integer","minimum":1,"maximum":64},"height":{"type":"integer","minimum":1,"maximum":64}}})");
}

void plMcpGpuDumpTool::Execute(plStringView tool, const plVariantDictionary& args, plMcpToolResult& result)
{
  if (tool == "engine_gpu_dump")
  {
    auto* connection = plEditorEngineProcessConnection::GetSingleton();
    if (!connection || !connection->IsEngineSetup() || connection->IsProcessCrashed())
    {
      result.SetError("The engine process is not ready.");
      return;
    }
    plSimpleConfigMsgToEngine request;
    request.m_sWhatToDo = "GpuDump";
    plVariantDictionary options;
    for (const char* key : {"root", "view", "compare"})
      if (args.Contains(key))
        options[key] = Arg(args, key);
    if (args.Contains("timingFrames"))
      options["timingFrames"] = Int(args, "timingFrames", 32);
    {
      plMcpJsonWriter writer;
      writer.WriteVariant(options);
      request.m_sPayload = writer.GetResult();
    }
    connection->SendMessage(&request);
    plString path, error;
    plUInt64 bytes = 0;
    plUInt32 anomalies = 0;
    auto callback = [&](plProcessMessage* message) -> bool
    {
      auto* response = plDynamicCast<plGpuDumpResponseToEditor*>(message);
      if (!response)
        return false;
      path = response->m_sPath;
      error = response->m_sError;
      bytes = response->m_uiBytes;
      anomalies = response->m_uiAnomalies;
      return true;
    };
    plProcessCommunicationChannel::WaitForMessageCallback cb = callback;
    if (connection->WaitForMessage(plGetStaticRTTI<plGpuDumpResponseToEditor>(), plTime::MakeFromSeconds(plMath::Clamp(Int(args, "timeoutSeconds", 300), 1, 600)), &cb).Failed())
    {
      result.SetError(connection->IsProcessCrashed() || !connection->IsEngineSetup()
                        ? "The engine process stopped before the GPU dump completed. Check engine_status and the engine log; partial output is retained."
                        : "GPU dump response timed out. The engine may still be writing; check gpu_dump_list and the engine log before retrying.");
      return;
    }
    if (!error.IsEmpty())
    {
      result.SetError(error);
      return;
    }
    const plString root = Register(path);
    plStringBuilder digest;
    if (ReadText(SafeFile(root, "digest.md"), 4 * 1024 * 1024, digest).Failed())
    {
      result.SetError("Capture completed, but its digest could not be opened.");
      return;
    }
    plVariantDictionary output;
    output["path"] = root;
    output["sizeBytes"] = static_cast<double>(bytes);
    output["anomalyCount"] = static_cast<plInt32>(anomalies);
    output["digest"] = plString(digest);
    if (auto* server = plMcpServer::GetInstance())
    {
      plStringBuilder viewer;
      viewer.SetFormat("http://127.0.0.1:{}/gpudump/{}/index.html", server->GetPort(), PercentEncode(plPathUtils::GetFileNameAndExtension(root)));
      output["viewer"] = plString(viewer);
    }
    Result(result, output);
    return;
  }
  if (tool == "gpu_dump_list")
  {
    plString directory = Arg(args, "directory");
    if (directory.IsEmpty())
    {
      plStringBuilder path;
      if (plFileSystem::ResolvePath(":appdata/GpuDumps", &path, nullptr).Succeeded())
        directory = path;
    }
    if (directory.IsEmpty())
    {
      result.SetError("The default dump directory could not be resolved. Supply directory explicitly.");
      return;
    }
    // the folders, newest first
    plDynamicArray<plFileStats> entries;
    plOSFile::GatherAllItemsInFolder(entries, directory, plFileSystemIteratorFlags::ReportFolders);
    entries.Sort([](const plFileStats& a, const plFileStats& b)
      { return a.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond) > b.m_LastModificationTime.GetInt64(plSIUnitOfTime::Microsecond); });
    plVariantArray dumps;
    for (const plFileStats& entry : entries)
    {
      if (entry.m_sName.EndsWith(".partial"))
        continue;
      plStringBuilder entryPath;
      entry.GetFullPath(entryPath);
      const plString root = Register(entryPath);
      const plString* registered = nullptr;
      if (root.IsEmpty() || !s_Roots.TryGetValue(plPathUtils::GetFileNameAndExtension(root), registered) || *registered != root)
        continue;
      plVariantDictionary dumpEntry;
      dumpEntry["name"] = entry.m_sName;
      dumpEntry["path"] = root;
      dumps.PushBack(dumpEntry);
      if (dumps.GetCount() == 200)
        break;
    }
    plVariantDictionary output;
    output["dumps"] = dumps;
    Result(result, output);
    return;
  }
  const plString root = Register(Arg(args, "path"));
  plJSONReader json;
  if (ReadDump(root, json).Failed() || JsonInt(json.GetTopLevelObject(), "schemaVersion") != 1)
  {
    result.SetError("The path must name a completed GPU dump with schemaVersion 1.");
    return;
  }
  const plVariantDictionary& dump = json.GetTopLevelObject();
  if (tool == "gpu_dump_pass")
  {
    int id = Int(args, "pass", -1);
    if (const plVariantArray* passes = JsonArray(dump, "passes"))
      for (const plVariant& p : *passes)
      {
        const plVariantDictionary* pass = JsonObject(p);
        if (pass == nullptr || JsonInt(*pass, "index") != id)
          continue;
        plVariantDictionary output = *pass;
        plVariantArray blobs;
        plHashSet<int> ids;
        if (const plVariantArray* commands = JsonArray(output, "commands"))
          for (const plVariant& command : *commands)
          {
            const plVariantDictionary* c = JsonObject(command);
            if (c == nullptr)
              continue;
            if (c->Contains("blob"))
              ids.Insert(JsonInt(*c, "blob"));
            if (const plVariantArray* bindings = JsonArray(*c, "bindings"))
              for (const plVariant& binding : *bindings)
                if (const plVariantDictionary* b = JsonObject(binding); b != nullptr && b->Contains("blob"))
                  ids.Insert(JsonInt(*b, "blob"));
          }
        if (const plVariantArray* constants = JsonArray(dump, "constants"))
          for (const plVariant& blob : *constants)
            if (const plVariantDictionary* b = JsonObject(blob); b != nullptr && ids.Contains(JsonInt(*b, "index")))
              blobs.PushBack(blob);
        output["constantBlobs"] = blobs;
        Result(result, output);
        return;
      }
    result.SetError("No such pass in this dump.");
    return;
  }
  int resourceId = Int(args, "resource", -1), versionId = Int(args, "version", -1);
  const plVariantDictionary* pResource = nullptr;
  const plVariantDictionary* pVersion = nullptr;
  if (const plVariantArray* resources = JsonArray(dump, "resources"))
    for (const plVariant& r : *resources)
      if (const plVariantDictionary* object = JsonObject(r); object != nullptr && JsonInt(*object, "index") == resourceId)
        pResource = object;
  if (const plVariantArray* versions = JsonArray(dump, "versions"))
    for (const plVariant& v : *versions)
      if (const plVariantDictionary* object = JsonObject(v); object != nullptr && JsonInt(*object, "resource") == resourceId && (versionId < 0 || JsonInt(*object, "version") == versionId))
        pVersion = object;
  if (pResource == nullptr || pResource->IsEmpty() || pVersion == nullptr || pVersion->IsEmpty())
  {
    result.SetError("Resource or version was not found.");
    return;
  }
  const plVariantDictionary& resource = *pResource;
  const plVariantDictionary& version = *pVersion;
  if (tool == "gpu_dump_stats")
  {
    plVariantDictionary output;
    output["resource"] = resource;
    output["version"] = version;
    Result(result, output);
    return;
  }
#if PL_ENABLED(PL_COMPILE_FOR_DEVELOPMENT)
  int mip = Int(args, "mip", 0), slice = Int(args, "slice", 0), z = Int(args, "z", 0), x = Int(args, "x", 0), y = Int(args, "y", 0), width = Int(args, "width", 1), height = Int(args, "height", 1);
  const plVariantDictionary* pSub = nullptr;
  if (const plVariantArray* subresources = JsonArray(version, "subresources"))
    for (const plVariant& s : *subresources)
      if (const plVariantDictionary* object = JsonObject(s); object != nullptr && JsonInt(*object, "mip") == mip && JsonInt(*object, "slice") == slice)
        pSub = object;
  if (pSub == nullptr || pSub->IsEmpty() || JsonString(resource, "kind") != "texture" || x < 0 || y < 0 || z < 0 || width < 1 || height < 1 || width > 64 || height > 64 || plInt64(x) + width > JsonInt(*pSub, "width") || plInt64(y) + height > JsonInt(*pSub, "height") || z >= JsonInt(*pSub, "depth"))
  {
    result.SetError("Invalid subresource or rectangle; maximum is 64 x 64 texels.");
    return;
  }
  const plVariantDictionary& sub = *pSub;
  const plVariantDictionary* pDecoded = JsonObject(sub, "decoded");
  const plVariantDictionary& rawInfo = pDecoded != nullptr ? *pDecoded : sub;
  int formatId = rawInfo.Contains("formatId") ? JsonInt(rawInfo, "formatId") : JsonInt(resource, "formatId");
  if (formatId <= 0 || formatId >= plGALResourceFormat::ENUM_COUNT)
  {
    result.SetError("Invalid format identifier.");
    return;
  }
  auto format = static_cast<plGALResourceFormat::Enum>(formatId);
  int stride = plGALResourceFormat::GetBitsPerElement(format) / 8;
  if (!stride || plGALResourceFormat::IsBlockCompressed(format))
  {
    result.SetError("Block-compressed texels require decompression; inspect the raw file or preview.");
    return;
  }
  const plString rawPath = SafeFile(root, JsonString(rawInfo, "file"));
  if (rawPath.IsEmpty())
  {
    result.SetError("Raw resource is missing or outside the dump.");
    return;
  }
  plDataBuffer raw;
  raw.SetCountUninitialized(width * height * stride);
  const plUInt64 offset = ((plUInt64(z) * JsonInt(sub, "height") + y) * JsonInt(sub, "width") + x) * stride;
  if (plGpuDumpUtils::ReadRawRows(rawPath, offset, width * stride, JsonInt(sub, "width") * stride, height, raw).Failed())
  {
    result.SetError("Raw file is truncated or cannot be decoded.");
    return;
  }
  plVariantArray rows;
  for (int row = 0; row < height; ++row)
  {
    plVariantArray pixels;
    for (int column = 0; column < width; ++column)
    {
      double values[4];
      if (!plGpuDumpUtils::DecodeTexel(format, raw.GetData() + (row * width + column) * stride, values))
      {
        result.SetError("No decoder for this format.");
        return;
      }
      plVariantArray channels;
      int count = format == plGALResourceFormat::D24S8 ? 2 : plGALResourceFormat::GetChannelCount(format);
      for (int c = 0; c < count; ++c)
      {
        if (std::isnan(values[c]))
          channels.PushBack(plString("NaN"));
        else if (std::isinf(values[c]))
          channels.PushBack(plString(values[c] < 0 ? "-Infinity" : "Infinity"));
        else
          channels.PushBack(values[c]);
      }
      pixels.PushBack(channels);
    }
    rows.PushBack(pixels);
  }
  plVariantDictionary output;
  output["resource"] = resourceId;
  output["version"] = JsonInt(version, "version");
  output["mip"] = mip;
  output["slice"] = slice;
  output["z"] = z;
  output["x"] = x;
  output["y"] = y;
  output["values"] = rows;
  Result(result, output);
#else
  result.SetError("Exact texel decoding requires a development build.");
#endif
}

bool plMcpGpuDumpTool::Serve(plMcpConnection& ref_connection, plStringView sPath)
{
  if (!sPath.StartsWith("/gpudump/"))
    return false;
  const plString decoded = PercentDecode(plStringView(sPath.GetStartPointer() + 9, sPath.GetEndPointer()));
  const char* szSlash = decoded.FindSubString("/");
  const plStringView name = szSlash != nullptr ? plStringView(decoded.GetData(), szSlash) : decoded.GetView();
  plStringView relative = szSlash != nullptr ? plStringView(szSlash + 1, decoded.GetData() + decoded.GetElementCount()) : plStringView();
  if (relative.IsEmpty())
    relative = "index.html";
  plString root;
  s_Roots.TryGetValue(name, root);
  const plString filePath = root.IsEmpty() ? plString() : SafeFile(root, relative);
  const plStringView type = GetContentType(plPathUtils::GetFileExtension(filePath));
  plUInt64 uiFileSize = 0;
  if (type.IsEmpty() || ref_connection.WriteFile(filePath, uiFileSize).Failed())
  {
    ref_connection.Write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    ref_connection.CloseWhenSent();
    return true;
  }
  plStringBuilder header;
  header.SetFormat("HTTP/1.1 200 OK\r\nContent-Type: {}\r\nX-Content-Type-Options: nosniff\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", type, uiFileSize);
  // the connection streams the file in chunks as the socket takes them, so a slow browser holds at most one chunk
  ref_connection.Write(header);
  ref_connection.CloseWhenSent();
  return true;
}
