"""SM86 -> GLSL, glsl.core.Builder methods for geometry programs (NVN stage 2): per-vertex inputs, outputs, OUT and
the declarations a geometry stage needs. A mixin: it uses the Builder's operand helpers (src_u, dst...) and state.

Semantics from Ryujinx (MIT, Ryujinx.Graphics.Shader: Instructions/InstEmitAttribute.cs Ald/Isberd/OutR,
InstEmitMove.cs S2R InvocationInfo, Translation/ShaderHeader.cs), attribute addresses from Mesa NAK nak_private.h
(MIT): 0x64 RT_ARRAY_INDEX (layer), 0x68 VIEWPORT_INDEX, 0x70 position, 0x80 + 16 i generic i.

What Orbitals needs (18/11/2026): Unreal's layer selector, which takes a triangle, copies each vertex's position and
generic 0 and writes generic 1.x (the instance index the vertex stage passes on) to the layer. It draws the 32 layers of
the colour-grading LUT; with the stage dropped only layer 0 was drawn and the tonemapper graded the frame to black.
"""

from program import Insn

from glsl.common import RZ, Unsupported, reg

# SPH word 3 bits 24-27 (Ryujinx OutputTopology): the GLSL output layout
OUT_TOPOLOGY = {1: "points", 6: "line_strip", 7: "triangle_strip"}


class GeomOps:
    def gs_vertex(self, i: Insn) -> str:
        """The vertex an ALD reads: Rb, an ISBERD result that is the vertex index (Ryujinx keeps the primitive index
        at zero); the console marks a vertex past the primitive's with 0x80000000, clamped here to the last one."""
        n = i.v("Rb")
        if n == RZ:
            raise Unsupported("ALD of a geometry program without a vertex")
        self.regs.add(n)
        return f"int(min({reg(n)}, uint(gl_in.length()) - 1u))"

    def gs_in_word(self, addr: int, v: str) -> str:
        if 0x70 <= addr < 0x80:
            return f"gl_in[{v}].gl_Position[{(addr - 0x70) >> 2}]"
        if 0x80 <= addr < 0x280:
            g, c = (addr - 0x80) // 16, ((addr - 0x80) >> 2) & 3
            self.in_attrs.add(g)
            return f"in_attr{g}[{v}][{c}]"
        raise Unsupported(f"ALD a[{addr:#x}] in a geometry program")

    def gs_attr(self, i: Insn) -> str:
        """ALD/AST of a geometry program. AST's Rc (the output vertex handle OUT returns) is not needed: GLSL writes the
        current vertex's outputs."""
        if i.v("Ra") != RZ or (i.op == "ALD" and i.sym("io") == "O"):
            raise Unsupported(f"{i.op} indexed or of outputs (geometry)")
        n = {"32": 1, "64": 2, "96": 3, "128": 4}.get(i.sym("sz"))
        if n is None:
            raise Unsupported(f"{i.op}.{i.sym('sz')}")
        base = i.v("Ra_offset")
        out = []
        if i.op == "ALD":
            v = self.gs_vertex(i)
            d = i.v("Rd")
            if d == RZ:
                return ""
            for k in range(n):
                self.regs.add(d + k)
                out.append(f"r{d + k} = floatBitsToUint({self.gs_in_word(base + 4 * k, v)});")
            return " ".join(out)
        for k in range(n):
            addr, r = base + 4 * k, i.v("Rb") + k if i.v("Rb") != RZ else RZ
            if r != RZ:
                self.regs.add(r)
            if addr == 0x64:
                out.append(f"gl_Layer = int({reg(r)});")
            elif addr == 0x68:
                out.append(f"gl_ViewportIndex = int({reg(r)});")
            else:
                target = self.attr_word(addr, True)
                if target:
                    out.append(f"{target} = uintBitsToFloat({reg(r)});")
        return " ".join(out)

    def gs_insn(self, i: Insn) -> str | None:
        """The instructions a geometry program has that others do not; None for the rest (Builder.insn)."""
        op = i.op
        if op in ("ALD", "AST"):
            return self.gs_attr(i)
        if op == "OUT":
            kind = i.sym("type")
            emit = kind in ("EMIT", "EMIT_THEN_CUT")
            cut = kind in ("CUT", "EMIT_THEN_CUT")
            if kind == "FINAL":   # the end of the program's output: nothing to do in GLSL
                return ""
            if not (emit or cut):
                raise Unsupported(f"OUT.{kind}")
            stmt = ("EmitVertex(); " if emit else "") + ("EndPrimitive(); " if cut else "")
            if i.has("Rd") and i.v("Rd") != RZ:   # the next vertex's handle: kept as the operand, as Ryujinx ignores it
                stmt += self.dst(i, self.src_u(i, "a"))
            return stmt.strip()
        if op == "ISBERD":
            # The offset in the internal stage buffer: PrimitiveIndex * VerticesPerPrimitive + VertexIndex, the
            # vertex index with the primitive index kept at zero (Ryujinx Isberd)
            return self.dst(i, self.src_u(i, "a"))
        if op == "S2R" and {"SR_INVOCATION_INFO", "SR29"} & set(i.f["SRa"].symbols or []):
            # input vertices << 16, primitive index (low byte) zero (Ryujinx S2R InvocationInfo)
            return self.dst(i, "(uint(gl_in.length()) << 16)")
        if op == "LEPC":   # its address only goes to a branch-unit register (BMOV), which nothing reads back
            return self.dst(i, f"{i.pc}u")
        if op == "BMOV" and i.cls.startswith("bmov_dst"):
            return ""
        return None

    def gs_head(self) -> list[str]:
        """Input and output layouts from the program header: triangles in (the only input the translator knows; the
        runtime attaches the stage to triangle draws only), the header's output topology and vertex count."""
        w = self.sph.words
        topo = OUT_TOPOLOGY.get((w[3] >> 24) & 0xF)
        count = w[4] & 0xFFF
        if topo is None or not count:
            raise Unsupported(f"geometry output topology {(w[3] >> 24) & 0xF}, {count} vertices")
        if (w[2] >> 24) & 0xFF not in (0, 1):
            raise Unsupported(f"geometry program with {(w[2] >> 24) & 0xFF} invocations per primitive")
        return ["layout(triangles) in;", f"layout({topo}, max_vertices = {count}) out;"]
