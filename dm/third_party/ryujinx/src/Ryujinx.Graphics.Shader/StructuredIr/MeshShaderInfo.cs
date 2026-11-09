using System;
using System.Collections.Generic;
using System.Linq;
using Ryujinx.Graphics.Shader.Translation;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    // nxnative (after azmasm/switch2mac, MIT): a Switch 2 (NVN2) task / mesh program, translated as compute (TranslationFlags.TaskShader / MeshShader) and emitted as a
    // SPV_EXT_mesh_shader TaskEXT / MeshEXT entry point (CodeGen/Spirv/SpirvGenerator.cs GenerateMeshEntry). Its stage memory (Instructions/InstEmitAmpereMesh.cs) is,
    // by the address space each access names:
    //   the task memory: written by a task (+4 = the mesh workgroups to launch, payload from +0x10), read by its mesh workgroups (+0 their own index among
    //     those, then the payload). One TaskPayloadWorkgroupEXT variable, PayloadWords words (the layout's payload bytes, rounded up to a power of two: addresses
    //     are masked); a mesh program declares it only when it reads the payload
    //   the per-vertex planes, mesh: one 0x80-byte plane per output component in chunks of 32 vertices (vertex v, plane p at (v / 32) x planes x 0x80 + p x 0x80 +
    //     (v % 32) x 4), then the per-primitive values in chunks of 32 primitives (primitive t, value n at PrimitiveBaseWord x 4 + (t / 32) x values x 0x80 + n x 0x80 +
    //     (t % 32) x 4). Its stores go straight into the mesh outputs (CodeGenContext.MeshPlaneStore); the memory itself is one word nothing reads
    //   the primitive area, mesh: byte 3 the primitive count, 4 + n t ... primitive t's n vertex indices (n = PrimitiveVertices: 3 for triangles, 1 for points: Zelda: Ocarina
    //     of Time's point meshes store one byte at 4 + t). One word per byte (each store a whole word, so no byte merging between invocations), PrimitiveWords words, addresses masked
    // The layout (IGpuAccessor.QueryMeshShaderLayout, the job's "mesh"): max vertices, max primitives, payload bytes, the per-vertex plane codes in the order of the
    // planes (the SPH's output map in attribute order: PointSizePlane the point size, 0-3 position x y z w, 4 + 4 g + c generic g component c), then optionally -1 and the per-primitive value codes (same numbering,
    // generic only; -3 for a value the program writes that no output takes: the count sets the chunk stride, values x 0x80), then optionally -2 and the generic locations to declare as per-vertex outputs although the program writes none of their components (the
    // pixel shader reads them: they hold (0, 0, 0, 1))
    class MeshShaderInfo
    {
        public const string PayloadName = "task_payload";

        // A per-primitive value no output takes (its stores are dropped)
        public const int NoOutput = -3;

        // The point size's plane code (the SPH output map's system value at attribute 0x6c, byte 0x35 bit 3, which comes before position); per vertex only
        public const int PointSizePlane = 4 + 4 * 32;

        public bool IsTask { get; }
        public bool IsMesh => !IsTask;
        public int MaxVertices { get; }
        public int MaxPrimitives { get; }
        public int PayloadBytes { get; }
        public int PayloadWords { get; }
        public int[] Planes { get; }
        public int[] PrimitivePlanes { get; }
        public int[] PadLocations { get; }

        public int PrimitiveWords { get; }

        // Vertices per output primitive (IGpuAccessor.QueryMeshShaderTopology, the control section's +0x5f): 3 triangles, 1 points
        public int PrimitiveVertices { get; }
        public bool Points => IsMesh && PrimitiveVertices == 1;

        // A mesh program's GL depth rewrite (IGpuAccessor.QueryMeshShaderGlDepthSpecId): the SpecId of its bool constant, -1 for none
        public int GlDepthSpecId { get; init; } = -1;

        public int PayloadMemoryId { get; private set; } = -1;
        public int PlaneMemoryId { get; private set; } = -1;
        public int PrimitiveMemoryId { get; private set; } = -1;

        public MeshShaderInfo(bool isTask, int[] layout, int primitiveVertices = 3)
        {
            IsTask = isTask;
            PrimitiveVertices = primitiveVertices;

            if (layout == null || layout.Length < 3)
            {
                throw new NotSupportedException($"{(isTask ? "task" : "mesh")} program without a mesh layout (job \"mesh\": max vertices, max primitives, payload bytes, planes ...)");
            }

            MaxVertices = layout[0];
            MaxPrimitives = layout[1];
            PayloadBytes = layout[2];

            int primSplit = Array.IndexOf(layout, -1, 3);
            int padSplit = Array.IndexOf(layout, -2, 3);
            int end = layout.Length;
            int planeEnd = primSplit >= 0 ? primSplit : padSplit >= 0 ? padSplit : end;
            int primEnd = padSplit >= 0 ? padSplit : end;

            Planes = layout[3..planeEnd];
            PrimitivePlanes = primSplit >= 0 ? layout[(primSplit + 1)..primEnd] : Array.Empty<int>();
            PadLocations = padSplit >= 0 ? layout[(padSplit + 1)..end] : Array.Empty<int>();

            if (IsMesh)
            {
                if (MaxVertices <= 0 || MaxVertices > 256 || MaxPrimitives <= 0 || MaxPrimitives > 256)
                {
                    throw new NotSupportedException($"mesh program with {MaxVertices} vertices / {MaxPrimitives} primitives at most (1-256 each)");
                }

                if (Planes.Any(p => p < 0 || p > PointSizePlane) || PrimitivePlanes.Any(p => p != NoOutput && (p < 4 || p >= 4 + 4 * 32)) || PadLocations.Any(g => g < 0 || g >= 32) ||
                    Planes.Distinct().Count() != Planes.Length || PrimitivePlanes.Where(p => p != NoOutput).Distinct().Count() != PrimitivePlanes.Count(p => p != NoOutput))
                {
                    throw new NotSupportedException($"mesh layout with a plane code out of range or repeated ({string.Join(", ", layout)})");
                }

                if (PrimitiveVertices is not (1 or 3))
                {
                    throw new NotSupportedException($"mesh program with {PrimitiveVertices} vertices per primitive (job \"mesh_topology\": 1 points, 3 triangles)");
                }

                // A point's size is the program's to write (VK_EXT_mesh_shader without maintenance5: a point mesh writes PointSize); a point program that writes none would
                // take NVN's point size state, which the library does not know
                if (Points && !Planes.Contains(PointSizePlane))
                {
                    throw new NotSupportedException($"point mesh program without a point size plane ({string.Join(", ", layout)})");
                }

                if (VertexLocations.Intersect(PrimitiveLocations).Any())
                {
                    throw new NotSupportedException($"mesh layout with a generic location both per vertex and per primitive ({string.Join(", ", layout)})");
                }
            }
            else if (Planes.Length != 0 || PrimitivePlanes.Length != 0 || PadLocations.Length != 0)
            {
                throw new NotSupportedException("task program with mesh output planes in its layout");
            }

            PayloadWords = PowerOfTwo(Math.Max(4, (PayloadBytes + 3) / 4));
            PrimitiveWords = PowerOfTwo(4 + PrimitiveVertices * Math.Max(1, MaxPrimitives));
        }

        public static int Chunks(int n)
        {
            return (n + 31) / 32;
        }

        // Words of one 32-vertex chunk, and of one 32-primitive chunk of the per-primitive values, which start after the vertex chunks
        public int VertexChunkWords => Planes.Length * 32;
        public int PrimitiveBaseWord => Chunks(MaxVertices) * VertexChunkWords;
        public int PrimitiveChunkWords => PrimitivePlanes.Length * 32;
        public int PlaneWords => PrimitiveBaseWord + Chunks(MaxPrimitives) * PrimitiveChunkWords;

        // The generic locations of the per-vertex outputs (the planes' and the padded ones) and of the per-primitive outputs
        public IEnumerable<int> VertexLocations => Planes.Where(p => p >= 4 && p < PointSizePlane).Select(p => (p - 4) >> 2).Concat(PadLocations).Distinct().OrderBy(g => g);
        public IEnumerable<int> PrimitiveLocations => PrimitivePlanes.Where(p => p != NoOutput).Select(p => (p - 4) >> 2).Distinct().OrderBy(g => g);

        private static int PowerOfTwo(int n)
        {
            int p = 1;
            while (p < n)
            {
                p <<= 1;
            }

            return p;
        }

        public void Declare(ShaderProperties properties)
        {
            if (IsTask)
            {
                DeclarePayload(properties);
            }
            else if (PlaneMemoryId < 0)
            {
                PlaneMemoryId = properties.AddSharedMemory(new MemoryDefinition("mesh_vertex_planes", AggregateType.Array | AggregateType.U32, 1));
                PrimitiveMemoryId = properties.AddSharedMemory(new MemoryDefinition("mesh_primitives", AggregateType.Array | AggregateType.U32, PrimitiveWords));
            }
        }

        // The task memory: a task's always, a mesh program's when it reads more than its word 0 (Instructions/InstEmitAmpereMesh.cs)
        public void DeclarePayload(ShaderProperties properties)
        {
            if (PayloadMemoryId < 0)
            {
                PayloadMemoryId = properties.AddSharedMemory(new MemoryDefinition(PayloadName, AggregateType.Array | AggregateType.U32, PayloadWords));
            }
        }

        public static MeshShaderInfo Create(TranslationFlags flags, IGpuAccessor gpuAccessor)
        {
            bool task = flags.HasFlag(TranslationFlags.TaskShader);
            if (!task && !flags.HasFlag(TranslationFlags.MeshShader))
            {
                return null;
            }

            if (task && flags.HasFlag(TranslationFlags.MeshShader))
            {
                throw new NotSupportedException("a program cannot be both a task and a mesh program");
            }

            return new MeshShaderInfo(task, gpuAccessor.QueryMeshShaderLayout(), task ? 3 : gpuAccessor.QueryMeshShaderTopology()) { GlDepthSpecId = gpuAccessor.QueryMeshShaderGlDepthSpecId() };
        }
    }
}
