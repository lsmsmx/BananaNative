// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project (Eglantine's lower_to_maxwell.h)
// SPDX-FileCopyrightText: Copyright 2026 azmasm (switch2mac's 0x923 / 0x927 lowering)
// SPDX-FileCopyrightText: Copyright 2026 seedds (this port)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ampere (NVN2, Switch 2) task / mesh stage memory: opcodes 0x923 (a load from the stage memory) and 0x927 (a store into the stage's output memory).
//
// The decoding of these two opcodes (which instruction word selects which memory, the operand fields, the access widths, the forms refused) is ported from
// switch2mac (azmasm, nxnative/dm/gfx/tools/nx2_lower/shader_recompiler/frontend/ampere/lower_to_maxwell.h, the 0x923 / 0x927 case), itself from Eglantine
// (djihads80/eglantine, src/shader_recompiler/frontend/ampere/), both GPL-3.0-or-later; this file is therefore GPL-3.0-or-later. Where switch2mac lowers each
// access to a Maxwell LDS / STS in a shared memory window ((window << 20) | offset) for its Ryujinx to map back, this emits the IR for the memory directly.
// The memories themselves and how they become SPIR-V (StructuredIr/MeshShaderInfo.cs, CodeGen/Spirv/SpirvGenerator.cs GenerateMeshEntry,
// CodeGenContext.MeshPlaneStore) are after switch2mac's MIT-licensed Ryujinx changes (Instructions/InstEmitMemory.cs MeshWindow, MeshShaderInfo.cs),
// as the rest of Ryujinx.Graphics.Shader is MIT.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later version. This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details. You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.

using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;

using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        private const int AStageLoad = 0x923;
        private const int AStageStore = 0x927;

        // The stage memories by the mode word (instruction bits 64..95), as nx2_lower numbers its windows:
        //   1 0x923 / 0x7800  the invocation's header word, address 0 (task and mesh): the workgroup's index
        //   2 0x923 / 0x3800  mesh: the task memory its task wrote (word 0 the mesh workgroup's index, records from 0x10)
        //   3 0x927 / 0xb800  task: the task memory (+4 = the mesh workgroups to launch)
        //   4 0x927 / 0xf800  mesh: the per-vertex planes (one 0x80-byte plane per output component, 32 vertices a chunk), then the per-primitive values
        //   5 0x927 / 0x8000  mesh: the primitive index bytes (4 + 3 t ...), 8-bit stores
        //   6 0x927 / 0x8800  mesh: the counts header (byte 3 = the primitive count), 8-bit stores
        private enum AStageWindow
        {
            None,
            Header,
            TaskMemoryRead,
            TaskMemoryReadBack,
            TaskMemoryWrite,
            VertexPlanes,
            PrimitiveIndices,
            PrimitiveHeader,
        }

        // 0x923 is ISBERD in the geometry and tessellation stages (InstEmitAmpereMem.cs AIsberd): outside a task / mesh program it stays that
        public static void RegisterAmpereMesh(AmpereRegistrar x)
        {
            x(AStageLoad, InstName.Isberd, AStageMemory, InstProps.None);
            x(AStageStore, InstName.Sts, AStageMemory, InstProps.None, writesRd: false);
        }

        // 0x923 Rd, [Ra + imm24] / 0x927 [Ra + imm24], Rb: Rd 16..23 (loads; stores have 0 there), Ra 24..31, Rb 32..39 (stores; loads have 0 there), the byte offset
        // imm24 at 40..63 (below 0x100000), the memory by the mode word at 64..95; bits 96..104 are 0. Every other form is refused
        public static void AStageMemory(EmitterContext context)
        {
            AmpereOp x = new(context.CurrOp);
            bool load = x.Opcode == AStageLoad;

            MeshShaderInfo mesh = context.TranslatorContext.Definitions.Stage == ShaderStage.Compute ? context.ResourceManager.Properties.Mesh : null;

            if (mesh == null)
            {
                if (load)
                {
                    AIsberd(context);
                    return;
                }

                throw AFail(context, "stage memory store outside a task / mesh program (job stage ts / ms)");
            }

            int mode = x.Bits(64, 96);
            int offset = x.Bits(40, 64);

            AStageWindow window = load
                ? mode switch { 0x7800 => AStageWindow.Header, 0x3800 => AStageWindow.TaskMemoryRead, 0xb800 => AStageWindow.TaskMemoryReadBack, _ => AStageWindow.None }
                : mode switch { 0xb800 => AStageWindow.TaskMemoryWrite, 0xf800 => AStageWindow.VertexPlanes, 0x8000 => AStageWindow.PrimitiveIndices, 0x8800 => AStageWindow.PrimitiveHeader, _ => AStageWindow.None };

            if (window == AStageWindow.None)
            {
                throw AFail(context, $"stage memory mode 0x{mode:x}");
            }

            if (offset >= 0x100000 || x.Bits(96, 105) != 0 || (load ? x.Rb != 0 : x.Rd != 0))
            {
                throw AFail(context, "stage memory form");
            }

            bool taskWindow = window is AStageWindow.TaskMemoryWrite or AStageWindow.TaskMemoryReadBack;
            bool meshWindow = window is AStageWindow.TaskMemoryRead or AStageWindow.VertexPlanes or AStageWindow.PrimitiveIndices or AStageWindow.PrimitiveHeader;

            if ((taskWindow && !mesh.IsTask) || (meshWindow && !mesh.IsMesh))
            {
                throw AFail(context, $"stage memory mode 0x{mode:x} in a {(mesh.IsTask ? "task" : "mesh")} program");
            }

            Operand address = x.Ra == RegisterConsts.RegisterZeroIndex ? Const(offset) : context.IAdd(Register(x.Ra, RegisterType.Gpr), Const(offset));

            switch (window)
            {
                case AStageWindow.Header:
                    // The workgroup's index (switch2mac: CtaId.x); only the word at 0 is known
                    if (x.Ra != RegisterConsts.RegisterZeroIndex || offset != 0)
                    {
                        throw AFail(context, "stage header word other than [RZ + 0]");
                    }

                    if (x.Rd != RegisterConsts.RegisterZeroIndex)
                    {
                        ASet(context, x, context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(0)));
                    }

                    return;

                case AStageWindow.TaskMemoryRead:
                    // Word 0 is the mesh workgroup's own: its index among those its task launched (Mario Kart World 1.8.0's ddc034e8e009ebfc reads [RZ + 0] and
                    // then the 40-byte record at 0x10 + 40 x that index, which its task fea1d6ca04512de1 compacts there), WorkgroupId.x in SPV_EXT_mesh_shader.
                    // switch2mac cleared it to 0 in the task instead, which hands every mesh workgroup the first record. The payload itself is declared only
                    // when a mesh program reads it, so a mesh program run without a task declares none
                    if (x.Rd != RegisterConsts.RegisterZeroIndex)
                    {
                        Operand index = context.Load(StorageKind.Input, IoVariable.CtaId, null, Const(0));

                        if (x.Ra == RegisterConsts.RegisterZeroIndex && offset < 4)
                        {
                            ASet(context, x, index);
                            return;
                        }

                        mesh.DeclarePayload(context.ResourceManager.Properties);
                        Operand word = context.ShiftRightU32(context.BitwiseAnd(address, Const(mesh.PayloadWords * 4 - 1)), Const(2));
                        Operand loaded = context.Load(StorageKind.SharedMemory, mesh.PayloadMemoryId, word);
                        ASet(context, x, x.Ra == RegisterConsts.RegisterZeroIndex ? loaded : context.ConditionalSelect(context.ICompareEqual(word, Const(0)), index, loaded));
                    }

                    return;

                case AStageWindow.TaskMemoryReadBack:
                    // Mode 0xb8 on a load (ISBERD in task shaders: Ocarina of Time's 5d3a096025e375a1): reads back a word the task shader wrote to its own payload
                    if (x.Rd != RegisterConsts.RegisterZeroIndex)
                    {
                        Operand word = context.ShiftRightU32(context.BitwiseAnd(address, Const(mesh.PayloadWords * 4 - 1)), Const(2));
                        Operand loaded = context.Load(StorageKind.SharedMemory, mesh.PayloadMemoryId, word);
                        ASet(context, x, loaded);
                    }

                    return;
            }

            Operand value = x.Rb == RegisterConsts.RegisterZeroIndex ? Const(0) : Register(x.Rb, RegisterType.Gpr);

            switch (window)
            {
                case AStageWindow.TaskMemoryWrite:
                    context.Store(StorageKind.SharedMemory, mesh.PayloadMemoryId, context.ShiftRightU32(context.BitwiseAnd(address, Const(mesh.PayloadWords * 4 - 1)), Const(2)), value);
                    break;

                case AStageWindow.VertexPlanes:
                    // Not masked: the word goes to MeshPlaneStore, which drops a word outside the vertex and per-primitive chunks
                    context.Store(StorageKind.SharedMemory, mesh.PlaneMemoryId, context.ShiftRightU32(address, Const(2)), value);
                    break;

                default:
                    // One word per byte address (MeshShaderInfo): the byte, zero-extended
                    context.Store(StorageKind.SharedMemory, mesh.PrimitiveMemoryId, context.BitwiseAnd(address, Const(mesh.PrimitiveWords - 1)), context.BitwiseAnd(value, Const(0xff)));
                    break;
            }
        }
    }
}
