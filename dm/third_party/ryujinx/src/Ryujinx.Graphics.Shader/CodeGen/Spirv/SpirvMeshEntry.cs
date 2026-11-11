using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using static Spv.Specification;

namespace Ryujinx.Graphics.Shader.CodeGen.Spirv
{
    using SpvInstruction = Spv.Generator.Instruction;
    using SpvLiteralInteger = Spv.Generator.LiteralInteger;

    // nxnative (after azmasm/switch2mac, MIT: their SpirvGenerator.GenerateMeshEntry, there for Metal through SPIRV-Cross; here for Vulkan 1.3 with SPV_EXT_mesh_shader):
    // the entry point of a Switch 2 task / mesh program (StructuredIr/MeshShaderInfo.cs). The translated program is function 0, a compute body whose early returns
    // stay legal. The entry point, "main", calls it between two workgroup barriers and hands its stage memory over:
    //   task (TaskEXT): before the body the task memory's word 1 (mesh workgroups to launch) is 0; after it OpEmitMeshTasksEXT(word 1, 1, 1, payload) ends every
    //     invocation (switch2mac also cleared word 0, which a mesh workgroup reads as its own index: InstEmitAmpereMesh.cs)
    //   mesh (MeshEXT): OpSetMeshOutputsEXT(max vertices, max primitives) first (no output is written before it), then the plane copy's defaults set (below), the
    //     body (its plane stores go to the plane copy, MeshPlaneStore below), then every primitive's indices from the primitive area: the program's indices (each
    //     clamped below max vertices) for t below the primitive count (byte 3); from there to max primitives a triangle is (0, 0, 0), degenerate, and a point (which
    //     cannot be degenerate) is index 0 with CullPrimitiveEXT set (MeshShaderInfo.PrimitiveVertices: 3 triangles, 1 points); then each invocation
    //     copies its own vertices and primitives (lid, lid + threads, ...) from the plane copy to the outputs, one vec4 per output, with a GL depth constant
    //     (MeshShaderInfo.GlDepthSpecId) z' = z / 2 + w / 2 of every vertex when it is true, as vk_android_spv.gl_depth does a vertex shader's
    // The plane copy: one workgroup u32 array per side, word v x planes + n for vertex v's plane n (MeshShaderInfo.Planes order; the per-primitive values the same),
    // starting as (0, 0, 0, 1) per output component as an unwritten one reads on the hardware. A plane store is then one dynamic store, and the outputs are written
    // once, after the last barrier. Before it (2026-11-07), each plane store went straight to the outputs: a chain of one guarded store per plane into the output
    // arrays at a vertex the lane decides, inlined at every store of the body (56 times in a 28-plane mesh program of Ocarina of Time); Qualcomm's compiler ran out
    // of memory on it once the lane was not constant (docs/reference/shaders.md, "Mesh plane stores into outputs exhaust Qualcomm's compiler"). The copy takes
    // 4 x (max vertices x planes + max primitives x values) bytes of workgroup memory; a program for which it and its own shared memory and payload exceed
    // MeshSharedBudget keeps the direct stores (MeshPlaneStore's old form, with the z / w copy for the GL depth rewrite)
    // Outputs: the position in a per-vertex block array (BuiltIn Position, Invariant under relaxed precision, then BuiltIn PointSize when the planes have the point size), one vec4 array per per-vertex generic location at Location g (as this translator's vertex
    // shaders number theirs), one vec4 array per per-primitive location at Location g decorated PerPrimitiveEXT, PrimitiveTriangleIndicesEXT uvec3[max primitives]
    // (points: PrimitivePointIndicesEXT uint[max primitives] and CullPrimitiveEXT bool[max primitives], OutputPoints).
    // SPIR-V 1.4: the entry point lists every global variable.
    // Dropped from switch2mac's Metal path: the per-vertex outputs at every location 0-5 whether written or not (Metal refused a mesh pipeline missing a fragment
    // input; here only the layout's padded locations are added), the 1024 mesh workgroup bound of a Metal object function (here 65535, Vulkan's
    // maxMeshWorkGroupCount minimum), SetMeshOutputsEXT after the body with the program's own count (Vulkan: before any output write; the unused primitives are
    // degenerate instead)
    static class SpirvMeshEntry
    {
        private const uint MaxMeshWorkgroups = 65535;

        // Workgroup memory a mesh program may use with its task payload: VK_EXT_mesh_shader's guaranteed minimum of maxMeshSharedMemorySize and
        // maxMeshPayloadAndSharedMemorySize (28 KB; Adreno 840 / Qualcomm's driver: 32 KB). The minimum, so that a library is the same for every device
        private const int MeshSharedBudget = 28672;

        // Workgroup bytes the program declares itself (its shared memory, the primitive area; not the task payload or memory moved to the spill buffer)
        private static int SharedBytes(CodeGenContext context)
        {
            int bytes = 0;
            foreach ((int id, MemoryDefinition memory) in context.Properties.SharedMemories)
            {
                if (context.Properties.SharedSpill?.MemoryId != id && memory.Name != MeshShaderInfo.PayloadName)
                {
                    bytes += memory.Type.GetSizeInBytes() * Math.Max(1, memory.ArrayLength);
                }
            }

            return bytes;
        }

        // The value an output component holds before the program writes it: (0, 0, 0, 1) (plane code: 0-3 position, 4 + 4 g + c generic g component c)
        private static float DefaultComponent(int code) => (code & 3) == 3 ? 1f : 0f;

        public static void Generate(CodeGenContext context, MeshShaderInfo mesh)
        {
            context.AddCapability(Capability.MeshShadingEXT);
            context.AddExtension("SPV_EXT_mesh_shader");

            SpvInstruction tVoid = context.TypeVoid(), tU32 = context.TypeU32(), tF32 = context.TypeFP32(), tBool = context.TypeBool();
            SpvInstruction tVec4 = context.TypeVector(tF32, 4), tUVec3 = context.TypeVector(tU32, 3);
            SpvInstruction U(uint v) => context.Constant(tU32, v);
            uint threads = (uint)Math.Max(1, context.Definitions.ComputeLocalSizeX * context.Definitions.ComputeLocalSizeY * context.Definitions.ComputeLocalSizeZ);
            List<SpvInstruction> io = new(context.GetMainInterface());

            SpvInstruction lidVar = context.Variable(context.TypePointer(StorageClass.Input, tU32), StorageClass.Input);
            context.Decorate(lidVar, Decoration.BuiltIn, (SpvLiteralInteger)BuiltIn.LocalInvocationIndex);
            context.AddGlobalVariable(lidVar);
            io.Add(lidVar);

            SpvInstruction payload = mesh.PayloadMemoryId >= 0 ? context.SharedMemories[mesh.PayloadMemoryId] : null;
            SpvInstruction pPayload = context.TypePointer(StorageClass.TaskPayloadWorkgroupEXT, tU32);
            SpvInstruction pWg = context.TypePointer(StorageClass.Workgroup, tU32);
            SpvInstruction pOutF32 = context.TypePointer(StorageClass.Output, tF32), pOutVec4 = context.TypePointer(StorageClass.Output, tVec4);

            SpvInstruction posVar = null, idxVar = null, cullVar = null;
            bool points = mesh.Points;
            bool pointSize = mesh.IsMesh && mesh.Planes.Contains(MeshShaderInfo.PointSizePlane);
            Dictionary<int, SpvInstruction> generics = new(), primGenerics = new();

            if (mesh.IsMesh)
            {
                SpvInstruction perVertex = pointSize ? context.TypeStruct(true, tVec4, tF32) : context.TypeStruct(true, tVec4);
                context.Decorate(perVertex, Decoration.Block);
                context.MemberDecorate(perVertex, 0, Decoration.BuiltIn, (SpvLiteralInteger)BuiltIn.Position);
                // Invariant under relaxed precision, as the vertex stages' gl_Position (Declarations.cs): a mesh program and another that computes the same position
                // (a depth prepass, then a pass that tests EQUAL against it) must get the same value, which relaxed precision does not promise otherwise
                if (context.HostCapabilities.ReducedPrecision)
                {
                    context.MemberDecorate(perVertex, 0, Decoration.Invariant);
                }
                if (pointSize)
                {
                    context.MemberDecorate(perVertex, 1, Decoration.BuiltIn, (SpvLiteralInteger)BuiltIn.PointSize);
                }
                posVar = context.Variable(context.TypePointer(StorageClass.Output, context.TypeArray(perVertex, U((uint)mesh.MaxVertices))), StorageClass.Output);
                context.Name(posVar, "mesh_position");
                context.AddGlobalVariable(posVar);
                io.Add(posVar);

                foreach (int g in mesh.VertexLocations)
                {
                    SpvInstruction v = context.Variable(context.TypePointer(StorageClass.Output, context.TypeArray(tVec4, U((uint)mesh.MaxVertices))), StorageClass.Output);
                    context.Decorate(v, Decoration.Location, (SpvLiteralInteger)g);
                    context.Name(v, $"mesh_out{g}");
                    context.AddGlobalVariable(v);
                    generics[g] = v;
                    io.Add(v);
                }

                foreach (int g in mesh.PrimitiveLocations)
                {
                    SpvInstruction v = context.Variable(context.TypePointer(StorageClass.Output, context.TypeArray(tVec4, U((uint)mesh.MaxPrimitives))), StorageClass.Output);
                    context.Decorate(v, Decoration.Location, (SpvLiteralInteger)g);
                    context.Decorate(v, Decoration.PerPrimitiveEXT);
                    context.Name(v, $"mesh_prim{g}");
                    context.AddGlobalVariable(v);
                    primGenerics[g] = v;
                    io.Add(v);
                }

                idxVar = context.Variable(context.TypePointer(StorageClass.Output, context.TypeArray(points ? tU32 : tUVec3, U((uint)mesh.MaxPrimitives))), StorageClass.Output);
                context.Decorate(idxVar, Decoration.BuiltIn, (SpvLiteralInteger)(points ? BuiltIn.PrimitivePointIndicesEXT : BuiltIn.PrimitiveTriangleIndicesEXT));
                context.AddGlobalVariable(idxVar);
                io.Add(idxVar);

                if (points)
                {
                    cullVar = context.Variable(context.TypePointer(StorageClass.Output, context.TypeArray(tBool, U((uint)mesh.MaxPrimitives))), StorageClass.Output);
                    context.Decorate(cullVar, Decoration.BuiltIn, (SpvLiteralInteger)BuiltIn.CullPrimitiveEXT);
                    context.Decorate(cullVar, Decoration.PerPrimitiveEXT);
                    context.AddGlobalVariable(cullVar);
                    io.Add(cullVar);
                }
            }

            SpvInstruction glDepth = null;
            if (mesh.IsMesh && mesh.GlDepthSpecId >= 0)
            {
                glDepth = context.DeclareSpecConstantFalse(tBool);
                context.Decorate(glDepth, Decoration.SpecId, (SpvLiteralInteger)mesh.GlDepthSpecId);
                context.Name(glDepth, "dm_gl_depth");
            }

            // The GL depth rewrite reads z and w back, which a mesh program cannot do with its outputs (VUID-StandaloneSpirv-MeshEXT-07107): their plane stores
            // also go to a workgroup copy, z then w of each vertex, starting as (0, 1)
            // The plane copy (see the header), when it fits
            int vertexSlots = mesh.Planes.Length, primSlots = mesh.PrimitivePlanes.Length;
            int copyBytes = 4 * (mesh.MaxVertices * vertexSlots + mesh.MaxPrimitives * primSlots);
            int payloadBytes = mesh.PayloadMemoryId >= 0 ? 4 * mesh.PayloadWords : 0;
            bool copied = mesh.IsMesh && context.HasMeshPlaneStore && SharedBytes(context) + copyBytes + payloadBytes <= MeshSharedBudget;
            SpvInstruction vertexCopy = null, primCopy = null;
            if (copied && vertexSlots != 0)
            {
                vertexCopy = context.Variable(context.TypePointer(StorageClass.Workgroup, context.TypeArray(tU32, U((uint)(mesh.MaxVertices * vertexSlots)))), StorageClass.Workgroup);
                context.Name(vertexCopy, "mesh_vertex_copy");
                context.AddGlobalVariable(vertexCopy);
            }

            if (copied && primSlots != 0)
            {
                primCopy = context.Variable(context.TypePointer(StorageClass.Workgroup, context.TypeArray(tU32, U((uint)(mesh.MaxPrimitives * primSlots)))), StorageClass.Workgroup);
                context.Name(primCopy, "mesh_primitive_copy");
                context.AddGlobalVariable(primCopy);
            }

            SpvInstruction zw = null;
            if (!copied && glDepth != null && mesh.Planes.Contains(2) && mesh.Planes.Contains(3))
            {
                zw = context.Variable(context.TypePointer(StorageClass.Workgroup, context.TypeArray(tU32, U((uint)mesh.MaxVertices * 2))), StorageClass.Workgroup);
                context.Name(zw, "mesh_position_zw");
                context.AddGlobalVariable(zw);
            }

            (_, SpvInstruction body) = context.GetFunction(0);
            SpvInstruction wrapper = context.Function(tVoid, FunctionControlMask.MaskNone, context.TypeFunction(tVoid, false));
            context.Name(wrapper, "main");
            context.AddFunction(wrapper);
            context.AddLabel(context.Label());

            void Barrier() => context.ControlBarrier(U((uint)Scope.Workgroup), U((uint)Scope.Workgroup), U((uint)(MemorySemanticsMask.AcquireRelease | MemorySemanticsMask.WorkgroupMemory)));

            void Guarded(SpvInstruction cond, Action then)
            {
                SpvInstruction thenLabel = context.Label(), mergeLabel = context.Label();
                context.SelectionMerge(mergeLabel, SelectionControlMask.MaskNone);
                context.BranchConditional(cond, thenLabel, mergeLabel);
                context.AddLabel(thenLabel);
                then();
                context.Branch(mergeLabel);
                context.AddLabel(mergeLabel);
            }

            // Each invocation takes elements lid, lid + threads, ... below count (unrolled: count / threads is at most 256)
            void ForEach(SpvInstruction lidValue, int count, Action<SpvInstruction> then)
            {
                for (uint k = 0; k * threads < count; k++)
                {
                    SpvInstruction t = k == 0 ? lidValue : context.IAdd(tU32, lidValue, U(k * threads));
                    if ((k + 1) * threads <= count)
                    {
                        then(t);
                    }
                    else
                    {
                        Guarded(context.ULessThan(tBool, t, U((uint)count)), () => then(t));
                    }
                }
            }

            SpvInstruction lid = context.Load(tU32, lidVar);
            SpvInstruction first = context.IEqual(tBool, lid, U(0));
            SpvInstruction init = context.ConstantComposite(tVec4, context.Constant(tF32, 0f), context.Constant(tF32, 0f), context.Constant(tF32, 0f), context.Constant(tF32, 1f));

            if (mesh.IsTask)
            {
                Guarded(first, () => context.Store(context.AccessChain(pPayload, payload, U(1)), U(0)));
            }
            else
            {
                context.SetMeshOutputsEXT(U((uint)mesh.MaxVertices), U((uint)mesh.MaxPrimitives));

                SpvInstruction prims = context.SharedMemories[mesh.PrimitiveMemoryId];
                Guarded(first, () => context.Store(context.AccessChain(pWg, prims, U(3)), U(0)));

                // The plane copy starts as the outputs' defaults; its copy-out writes every output whole
                void InitCopy(SpvInstruction copy, int count, int[] codes)
                {
                    ForEach(lid, count, i =>
                    {
                        SpvInstruction b = context.IMul(tU32, i, U((uint)codes.Length));
                        for (int n = 0; n < codes.Length; n++)
                        {
                            uint bits = DefaultComponent(codes[n]) == 1f ? 0x3f800000u : 0u;
                            context.Store(context.AccessChain(pWg, copy, n == 0 ? b : context.IAdd(tU32, b, U((uint)n))), U(bits));
                        }
                    });
                }

                if (vertexCopy != null)
                {
                    InitCopy(vertexCopy, mesh.MaxVertices, mesh.Planes);
                }

                if (primCopy != null)
                {
                    InitCopy(primCopy, mesh.MaxPrimitives, mesh.PrimitivePlanes);
                }

                // Without the plane copy, outputs start as (0, 0, 0, 1), as an unwritten component reads on the hardware (vk_android_spv.init_outputs gives a vertex
                // shader's the same): the position and the generic locations whose components the planes do not all name, and every per-primitive location
                bool posFull = Enumerable.Range(0, 4).All(c => mesh.Planes.Contains(c));
                List<SpvInstruction> vertexInit = generics.Where(kv => !Enumerable.Range(0, 4).All(c => mesh.Planes.Contains(4 + 4 * kv.Key + c))).Select(kv => kv.Value).ToList();
                if (!copied && (vertexInit.Count != 0 || !posFull))
                {
                    ForEach(lid, mesh.MaxVertices, v =>
                    {
                        if (!posFull)
                        {
                            context.Store(context.AccessChain(pOutVec4, posVar, v, U(0)), init);
                        }

                        foreach (SpvInstruction g in vertexInit)
                        {
                            context.Store(context.AccessChain(pOutVec4, g, v), init);
                        }
                    });
                }

                if (zw != null)
                {
                    ForEach(lid, mesh.MaxVertices, v =>
                    {
                        SpvInstruction e = context.IMul(tU32, v, U(2));
                        context.Store(context.AccessChain(pWg, zw, e), U(0));
                        context.Store(context.AccessChain(pWg, zw, context.IAdd(tU32, e, U(1))), U(0x3f800000));
                    });
                }

                if (!copied && primGenerics.Count != 0)
                {
                    ForEach(lid, mesh.MaxPrimitives, t =>
                    {
                        foreach (SpvInstruction g in primGenerics.Values)
                        {
                            context.Store(context.AccessChain(pOutVec4, g, t), init);
                        }
                    });
                }
            }

            Barrier();
            context.FunctionCall(tVoid, body);
            Barrier();

            if (mesh.IsTask)
            {
                SpvInstruction count = context.Load(tU32, context.AccessChain(pPayload, payload, U(1)));
                count = context.Select(tU32, context.ULessThan(tBool, count, U(MaxMeshWorkgroups)), count, U(MaxMeshWorkgroups));
                context.EmitMeshTasksEXT(count, U(1), U(1), payload);
            }
            else
            {
                SpvInstruction prims = context.SharedMemories[mesh.PrimitiveMemoryId];
                SpvInstruction primCount = context.Load(tU32, context.AccessChain(pWg, prims, U(3)));
                SpvInstruction pOutUVec3 = context.TypePointer(StorageClass.Output, tUVec3);
                SpvInstruction lastVertex = U((uint)mesh.MaxVertices - 1);

                SpvInstruction Index(SpvInstruction word)
                {
                    SpvInstruction w = context.BitwiseAnd(tU32, word, U((uint)mesh.PrimitiveWords - 1));
                    SpvInstruction b = context.Load(tU32, context.AccessChain(pWg, prims, w));
                    return context.Select(tU32, context.ULessThan(tBool, b, U((uint)mesh.MaxVertices)), b, lastVertex);
                }

                ForEach(lid, mesh.MaxPrimitives, t =>
                {
                    if (points)
                    {
                        SpvInstruction used = context.ULessThan(tBool, t, primCount);
                        SpvInstruction at = context.IAdd(tU32, t, U(4));
                        context.Store(context.AccessChain(context.TypePointer(StorageClass.Output, tU32), idxVar, t), context.Select(tU32, used, Index(at), U(0)));
                        context.Store(context.AccessChain(context.TypePointer(StorageClass.Output, tBool), cullVar, t), context.LogicalNot(tBool, used));
                        return;
                    }

                    SpvInstruction a = context.IAdd(tU32, context.IMul(tU32, t, U(3)), U(4));
                    SpvInstruction tri = context.CompositeConstruct(tUVec3, Index(a), Index(context.IAdd(tU32, a, U(1))), Index(context.IAdd(tU32, a, U(2))));
                    tri = context.Select(tUVec3, context.ULessThan(tBool, t, primCount), tri, context.ConstantComposite(tUVec3, U(0), U(0), U(0)));
                    context.Store(context.AccessChain(pOutUVec3, idxVar, t), tri);
                });

                // The GL depth convention (vk_android_spv.gl_depth's arithmetic for a vertex shader): z' = fma(z, 0.5, w * 0.5) when the constant is true
                if (zw != null)
                {
                    SpvInstruction half = context.Constant(tF32, 0.5f);
                    ForEach(lid, mesh.MaxVertices, v =>
                    {
                        SpvInstruction e = context.IMul(tU32, v, U(2));
                        SpvInstruction z = context.Bitcast(tF32, context.Load(tU32, context.AccessChain(pWg, zw, e)));
                        SpvInstruction w = context.Bitcast(tF32, context.Load(tU32, context.AccessChain(pWg, zw, context.IAdd(tU32, e, U(1)))));
                        context.Store(context.AccessChain(pOutF32, posVar, v, U(0), U(2)), context.Select(tF32, glDepth, context.GlslFma(tF32, z, half, context.FMul(tF32, w, half)), z));
                    });
                }

                // The plane copy to the outputs (after the barrier that follows the body: every plane store is in), one vec4 per output of each own vertex / primitive
                if (copied)
                {
                    SpvInstruction Component(SpvInstruction copy, int[] codes, SpvInstruction b, int code)
                    {
                        int n = Array.IndexOf(codes, code);
                        if (n < 0)
                        {
                            return context.Constant(tF32, DefaultComponent(code));
                        }

                        return context.Bitcast(tF32, context.Load(tU32, context.AccessChain(pWg, copy, n == 0 ? b : context.IAdd(tU32, b, U((uint)n)))));
                    }

                    SpvInstruction Vec4(SpvInstruction copy, int[] codes, SpvInstruction b, int code0) =>
                        context.CompositeConstruct(tVec4, Component(copy, codes, b, code0), Component(copy, codes, b, code0 + 1), Component(copy, codes, b, code0 + 2), Component(copy, codes, b, code0 + 3));

                    bool depth = glDepth != null && mesh.Planes.Contains(2) && mesh.Planes.Contains(3);
                    ForEach(lid, mesh.MaxVertices, v =>
                    {
                        SpvInstruction b = context.IMul(tU32, v, U((uint)vertexSlots));
                        SpvInstruction pos;
                        if (depth)
                        {
                            SpvInstruction x = Component(vertexCopy, mesh.Planes, b, 0), y = Component(vertexCopy, mesh.Planes, b, 1);
                            SpvInstruction z = Component(vertexCopy, mesh.Planes, b, 2), w = Component(vertexCopy, mesh.Planes, b, 3);
                            SpvInstruction half = context.Constant(tF32, 0.5f);
                            z = context.Select(tF32, glDepth, context.GlslFma(tF32, z, half, context.FMul(tF32, w, half)), z);
                            pos = context.CompositeConstruct(tVec4, x, y, z, w);
                        }
                        else
                        {
                            pos = Vec4(vertexCopy, mesh.Planes, b, 0);
                        }

                        context.Store(context.AccessChain(pOutVec4, posVar, v, U(0)), pos);
                        if (pointSize)
                        {
                            context.Store(context.AccessChain(pOutF32, posVar, v, U(1)), Component(vertexCopy, mesh.Planes, b, MeshShaderInfo.PointSizePlane));
                        }

                        foreach ((int g, SpvInstruction output) in generics)
                        {
                            context.Store(context.AccessChain(pOutVec4, output, v), Vec4(vertexCopy, mesh.Planes, b, 4 + 4 * g));
                        }
                    });

                    if (primGenerics.Count != 0)
                    {
                        ForEach(lid, mesh.MaxPrimitives, t =>
                        {
                            SpvInstruction b = context.IMul(tU32, t, U((uint)primSlots));
                            foreach ((int g, SpvInstruction output) in primGenerics)
                            {
                                context.Store(context.AccessChain(pOutVec4, output, t), Vec4(primCopy, mesh.PrimitivePlanes, b, 4 + 4 * g));
                            }
                        });
                    }
                }

                context.Return();
            }

            context.FunctionEnd();

            // The plane stores (MeshShaderInfo), into the plane copy (one store) or, without it, straight into the outputs: word w below the per-primitive base is vertex (w / chunk) x 32 + w % 32, plane (w % chunk) / 32
            // (chunk = 32 x planes); above it the same for the primitives and their values. Words outside both, and planes of no output, are dropped
            if (mesh.IsMesh && context.HasMeshPlaneStore)
            {
                SpvInstruction store = context.MeshPlaneStore;
                context.Name(store, "mesh_plane_store");
                context.AddFunction(store);
                SpvInstruction word = context.FunctionParameter(tU32), value = context.FunctionParameter(tU32);
                context.AddLabel(context.Label());
                SpvInstruction f = context.Bitcast(tF32, value);

                void Scatter(SpvInstruction w, int chunkWords, int count, int[] planes, Action<SpvInstruction, int> target)
                {
                    if (chunkWords == 0)
                    {
                        return;
                    }

                    SpvInstruction rem = context.UMod(tU32, w, U((uint)chunkWords));
                    SpvInstruction plane = context.ShiftRightLogical(tU32, rem, U(5));
                    SpvInstruction index = context.IAdd(tU32, context.IMul(tU32, context.UDiv(tU32, w, U((uint)chunkWords)), U(32)), context.BitwiseAnd(tU32, rem, U(31)));
                    Guarded(context.ULessThan(tBool, index, U((uint)count)), () =>
                    {
                        for (int n = 0; n < planes.Length; n++)
                        {
                            int p = n;
                            Guarded(context.IEqual(tBool, plane, U((uint)p)), () => target(index, planes[p]));
                        }
                    });
                }

                // Into the plane copy: element index x slots + plane, one dynamic store whatever the plane
                void ToCopy(SpvInstruction w, int chunkWords, int count, int slots, SpvInstruction copy)
                {
                    SpvInstruction rem = context.UMod(tU32, w, U((uint)chunkWords));
                    SpvInstruction index = context.IAdd(tU32, context.IMul(tU32, context.UDiv(tU32, w, U((uint)chunkWords)), U(32)), context.BitwiseAnd(tU32, rem, U(31)));
                    Guarded(context.ULessThan(tBool, index, U((uint)count)), () =>
                        context.Store(context.AccessChain(pWg, copy, context.IAdd(tU32, context.IMul(tU32, index, U((uint)slots)), context.ShiftRightLogical(tU32, rem, U(5)))), value));
                }

                SpvInstruction isVertex = context.ULessThan(tBool, word, U((uint)mesh.PrimitiveBaseWord));
                if (copied)
                {
                    if (vertexCopy != null)
                    {
                        Guarded(isVertex, () => ToCopy(word, mesh.VertexChunkWords, mesh.MaxVertices, vertexSlots, vertexCopy));
                    }

                    if (primCopy != null)
                    {
                        Guarded(context.LogicalNot(tBool, isVertex), () => ToCopy(context.ISub(tU32, word, U((uint)mesh.PrimitiveBaseWord)), mesh.PrimitiveChunkWords, mesh.MaxPrimitives, primSlots, primCopy));
                    }
                }
                else
                {
                    Guarded(isVertex, () => Scatter(word, mesh.VertexChunkWords, mesh.MaxVertices, mesh.Planes, (v, plane) =>
                    {
                        context.Store(plane == MeshShaderInfo.PointSizePlane
                            ? context.AccessChain(pOutF32, posVar, v, U(1))
                            : plane < 4
                            ? context.AccessChain(pOutF32, posVar, v, U(0), U((uint)plane))
                            : context.AccessChain(pOutF32, generics[(plane - 4) >> 2], v, U((uint)(plane & 3))), f);

                        if (zw != null && plane is 2 or 3)
                        {
                            context.Store(context.AccessChain(pWg, zw, context.IAdd(tU32, context.IMul(tU32, v, U(2)), U((uint)plane - 2))), value);
                        }
                    }));
                    if (mesh.PrimitivePlanes.Length != 0)
                    {
                        Guarded(context.LogicalNot(tBool, isVertex), () => Scatter(context.ISub(tU32, word, U((uint)mesh.PrimitiveBaseWord)), mesh.PrimitiveChunkWords, mesh.MaxPrimitives,
                            mesh.PrimitivePlanes, (t, plane) =>
                            {
                                if (plane != MeshShaderInfo.NoOutput)
                                {
                                    context.Store(context.AccessChain(pOutF32, primGenerics[(plane - 4) >> 2], t, U((uint)(plane & 3))), f);
                                }
                            }));
                    }
                }

                context.Return();
                context.FunctionEnd();
            }

            foreach (SpvInstruction global in context.GlobalVariables)   // SPIR-V 1.4: every global, not only the inputs and outputs
            {
                if (!io.Contains(global))
                {
                    io.Add(global);
                }
            }

            context.AddEntryPoint(mesh.IsTask ? ExecutionModel.TaskEXT : ExecutionModel.MeshEXT, wrapper, "main", io.ToArray());
            context.AddExecutionMode(wrapper, ExecutionMode.LocalSize,
                (SpvLiteralInteger)context.Definitions.ComputeLocalSizeX, (SpvLiteralInteger)context.Definitions.ComputeLocalSizeY, (SpvLiteralInteger)context.Definitions.ComputeLocalSizeZ);

            if (mesh.IsMesh)
            {
                context.AddExecutionMode(wrapper, ExecutionMode.OutputVertices, (SpvLiteralInteger)mesh.MaxVertices);
                context.AddExecutionMode(wrapper, ExecutionMode.OutputPrimitivesEXT, (SpvLiteralInteger)mesh.MaxPrimitives);
                context.AddExecutionMode(wrapper, points ? ExecutionMode.OutputPoints : ExecutionMode.OutputTrianglesEXT);
            }
        }
    }
}
