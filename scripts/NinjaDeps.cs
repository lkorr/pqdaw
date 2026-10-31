// Foreign-dependency check for Ninja's dependency log (scripts/build.ps1, T0.x-buildspeed2).
//
// All checkouts share one compiler cache (%LOCALAPPDATA%\pqdaw\sccache-shared) with each checkout root as an
// sccache base directory, so the same source compiled in two worktrees is one cache entry. A hit replays the
// /showIncludes output of the compile that filled the entry, which names THAT checkout's headers; Ninja records
// them as the object's dependencies and would then never rebuild the object when this checkout's headers change
// (T0.x-buildspeed-fix2/fix3; the 3d-sand-voxel project's "Phase 1b" poison check for the same bug).
//
// The object itself is right: the cache key covers the preprocessed source with the base directories stripped,
// so a hit means this checkout's headers had the same content. Only the recorded paths are wrong. So the check
// rewrites every dependency that lies in another pqdaw checkout to the same relative path in this one. A path that
// has no counterpart here (cannot happen for a true hit; possible for a corrupt entry) is reported instead, and
// build.ps1 deletes those objects and recompiles them once with SCCACHE_RECACHE=1, refusing to continue if any
// survive.
//
// .ninja_deps format (ninja src/deps_log.cc, version 4): "# ninjadeps\n", int32 version; then records, each a
// uint32 size whose top bit marks a deps record. Path record: the path, NUL-padded to 4 bytes, then uint32 ~id
// (ids are assigned in file order). Deps record: int32 output id, uint32 mtime low, uint32 mtime high, int32 input
// ids. A later deps record for the same output replaces an earlier one.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace Pq {
public sealed class NinjaDepsResult {
  public int Objects;                                    // outputs with a dependency record
  public int ForeignObjects;                             // outputs that had at least one foreign dependency
  public int Rewritten;                                  // of those, fully repaired by rewriting paths
  public List<string> Unmappable = new List<string>();   // outputs (relative to the build dir) left foreign
  public SortedSet<string> ForeignRoots = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
  public string Note = "";
}

public static class NinjaDeps {
  const string Signature = "# ninjadeps\n";

  static string Norm(string p) { return p.Replace('/', '\\').TrimEnd('\\'); }

  // checkoutRoots: every known pqdaw checkout (git worktree list), this one included. Worktrees that no longer
  // exist are recognised by their <main>\.claude\worktrees\<name> shape.
  public static NinjaDepsResult Fix(string buildDir, string myRoot, string[] checkoutRoots, bool write) {
    var r = new NinjaDepsResult();
    string logPath = Path.Combine(buildDir, ".ninja_deps");
    if (!File.Exists(logPath)) { r.Note = "no dependency log yet"; return r; }
    byte[] data = File.ReadAllBytes(logPath);
    if (data.Length < 16 || Encoding.ASCII.GetString(data, 0, 12) != Signature) { r.Note = "not a ninja deps log"; return r; }
    int version = BitConverter.ToInt32(data, 12);
    if (version != 4) { r.Note = "unsupported ninja deps log version " + version; return r; }

    var paths = new List<string>();
    var deps = new Dictionary<int, Tuple<uint, uint, int[]>>();
    var order = new List<int>();
    int pos = 16;
    while (pos + 4 <= data.Length) {
      uint head = BitConverter.ToUInt32(data, pos);
      bool isDeps = (head & 0x80000000u) != 0;
      int size = (int)(head & 0x7FFFFFFFu);
      if (size < 4 || pos + 4 + size > data.Length) break;  // truncated tail: Ninja ignores it too
      int b = pos + 4;
      if (isDeps) {
        int outId = BitConverter.ToInt32(data, b);
        uint lo = BitConverter.ToUInt32(data, b + 4), hi = BitConverter.ToUInt32(data, b + 8);
        int n = size / 4 - 3;
        var ins = new int[n];
        for (int i = 0; i < n; ++i) ins[i] = BitConverter.ToInt32(data, b + 12 + 4 * i);
        if (!deps.ContainsKey(outId)) order.Add(outId);
        deps[outId] = Tuple.Create(lo, hi, ins);
      } else {
        int len = size - 4;
        while (len > 0 && data[b + len - 1] == 0) --len;
        paths.Add(Encoding.UTF8.GetString(data, b, len));
      }
      pos += 4 + size;
    }
    r.Objects = deps.Count;

    string mine = Norm(Path.GetFullPath(myRoot));
    var roots = new List<string>();
    foreach (var c in checkoutRoots) if (!string.IsNullOrWhiteSpace(c)) roots.Add(Norm(Path.GetFullPath(c)));
    var removedWorktree = new Regex(@"^(.*\\\.claude\\worktrees\\[^\\]+)\\", RegexOptions.IgnoreCase);
    string bdir = Norm(Path.GetFullPath(buildDir));

    // Per path: its replacement (null: not foreign), or "" when it is foreign without a counterpart here.
    var mapped = new string[paths.Count];
    for (int i = 0; i < paths.Count; ++i) {
      string p = paths[i];
      string abs;
      try { abs = Norm(Path.GetFullPath(Path.IsPathRooted(p) ? p : Path.Combine(bdir, p))); }
      catch (Exception) { continue; }
      string best = null;
      foreach (var root in roots)
        if (abs.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase) && (best == null || root.Length > best.Length)) best = root;
      Match m = removedWorktree.Match(abs);
      if (m.Success && (best == null || m.Groups[1].Value.Length > best.Length)) best = m.Groups[1].Value;
      if (best == null || string.Equals(best, mine, StringComparison.OrdinalIgnoreCase)) continue;
      r.ForeignRoots.Add(best);
      string here = mine + abs.Substring(best.Length);
      if (!File.Exists(here)) { mapped[i] = ""; continue; }
      string rel = Path.GetRelativePath(bdir, here);
      mapped[i] = rel.Replace('\\', '/');
    }

    var newIds = new Dictionary<string, int>(StringComparer.Ordinal);
    var newPaths = new List<string>();
    Func<string, int> idOf = s => {
      int id;
      if (!newIds.TryGetValue(s, out id)) { id = newPaths.Count; newIds[s] = id; newPaths.Add(s); }
      return id;
    };
    var newDeps = new List<Tuple<int, uint, uint, int[]>>();
    foreach (int outId in order) {
      var d = deps[outId];
      bool foreign = false, bad = false;
      var ins = new List<int>(d.Item3.Length);
      foreach (int id in d.Item3) {
        if (id < 0 || id >= paths.Count) { bad = true; continue; }
        string to = mapped[id];
        if (to == null) { ins.Add(idOf(paths[id])); continue; }
        foreign = true;
        if (to.Length == 0) { bad = true; ins.Add(idOf(paths[id])); } else ins.Add(idOf(to));
      }
      if (outId < 0 || outId >= paths.Count) continue;
      if (foreign || bad) {
        if (foreign) r.ForeignObjects++;
        if (bad) r.Unmappable.Add(paths[outId]); else r.Rewritten++;
      }
      newDeps.Add(Tuple.Create(idOf(paths[outId]), d.Item1, d.Item2, ins.ToArray()));
    }
    if (!write || r.Rewritten == 0) return r;

    using (var ms = new MemoryStream())
    using (var w = new BinaryWriter(ms)) {
      w.Write(Encoding.ASCII.GetBytes(Signature));
      w.Write(4);
      for (int i = 0; i < newPaths.Count; ++i) {
        byte[] s = Encoding.UTF8.GetBytes(newPaths[i]);
        int pad = (4 - s.Length % 4) % 4;
        w.Write((uint)(s.Length + pad + 4));
        w.Write(s);
        for (int k = 0; k < pad; ++k) w.Write((byte)0);
        w.Write(~(uint)i);
      }
      foreach (var d in newDeps) {
        w.Write((uint)(4 * (3 + d.Item4.Length)) | 0x80000000u);
        w.Write(d.Item1);
        w.Write(d.Item2);
        w.Write(d.Item3);
        foreach (int id in d.Item4) w.Write(id);
      }
      w.Flush();
      string tmp = logPath + ".pqtmp";
      File.WriteAllBytes(tmp, ms.ToArray());
      File.Move(tmp, logPath, true);
    }
    return r;
  }
}
}
