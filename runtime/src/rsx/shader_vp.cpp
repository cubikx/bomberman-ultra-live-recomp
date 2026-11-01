// RSX vertex program microcode -> GLSL.
//
// Each instruction is 4 words and can issue one vector op and one scalar op.
// Both read their sources before either writes, so the translation computes
// both results into temporaries first, then performs the (masked,
// conditional) writes.
#include <sstream>

#include "shader.h"

namespace rsx::shader {
namespace {

u32 bits(u32 w, u32 lo, u32 n) { return (w >> lo) & ((1u << n) - 1); }

struct VPInsn {
	u32 d0, d1, d2, d3;
	// d0
	u32 addr_swz() const { return bits(d0, 0, 2); }
	u32 cond_swz(int c) const { return bits(d0, 8 - 2 * c, 2); }   // c: 0=x 1=y 2=z 3=w
	u32 cond() const { return bits(d0, 10, 3); }
	bool cond_test() const { return bits(d0, 13, 1); }
	bool cond_update() const { return bits(d0, 14, 1) || bits(d0, 29, 1); }
	u32 dst_tmp() const { return bits(d0, 15, 6); }
	bool src_abs(int n) const { return bits(d0, 21 + n, 1); }
	u32 addr_reg() const { return bits(d0, 24, 1); }
	u32 cond_reg() const { return bits(d0, 25, 1); }
	bool saturate() const { return bits(d0, 26, 1); }
	bool index_input() const { return bits(d0, 27, 1); }
	bool vec_result() const { return bits(d0, 30, 1); }
	// d1
	u32 input_src() const { return bits(d1, 8, 4); }
	u32 const_src() const { return bits(d1, 12, 10); }
	u32 vec_op() const { return bits(d1, 22, 5); }
	u32 sca_op() const { return bits(d1, 27, 5); }
	// d3
	bool end() const { return bits(d3, 0, 1); }
	bool index_const() const { return bits(d3, 1, 1); }
	u32 dst() const { return bits(d3, 2, 5); }
	u32 sca_dst_tmp() const { return bits(d3, 7, 6); }
	u32 vec_mask() const {   // xyzw -> bits 0..3
		return (bits(d3, 16, 1) << 0) | (bits(d3, 15, 1) << 1) | (bits(d3, 14, 1) << 2) | (bits(d3, 13, 1) << 3);
	}
	u32 sca_mask() const {
		return (bits(d3, 20, 1) << 0) | (bits(d3, 19, 1) << 1) | (bits(d3, 18, 1) << 2) | (bits(d3, 17, 1) << 3);
	}
	// 17-bit source operand n
	u32 src(int n) const {
		switch (n) {
		case 0: return bits(d2, 23, 9) | (bits(d1, 0, 8) << 9);
		case 1: return bits(d2, 6, 17);
		default: return bits(d3, 21, 11) | (bits(d2, 0, 6) << 11);
		}
	}
};

enum VecOp { V_NOP, V_MOV, V_MUL, V_ADD, V_MAD, V_DP3, V_DPH, V_DP4, V_DST, V_MIN, V_MAX, V_SLT, V_SGE,
             V_ARL, V_FRC, V_FLR, V_SEQ, V_SFL, V_SGT, V_SLE, V_SNE, V_STR, V_SSG, V_TXL = 0x19 };
enum ScaOp { S_NOP, S_MOV, S_RCP, S_RCC, S_RSQ, S_EXP, S_LOG, S_LIT, S_BRA, S_BRI, S_CAL, S_CLI, S_RET,
             S_LG2, S_EX2, S_SIN, S_COS, S_BRB, S_CLB, S_PSH, S_POP };

const char SWZ[] = "xyzw";

class VPTranslator {
public:
	VPTranslator(const u32* ucode, u32 start) : m_ucode(ucode), m_start(start) {}

	VertexProgram run() {
		VertexProgram out;
		std::ostringstream body;
		u32 pc = m_start;
		for (u32 n = 0; n < VP_INSTRUCTION_LIMIT; n++, pc++) {
			if (pc >= 512) {
				out.error = "ran past the end of program memory";
				return out;
			}
			VPInsn in{m_ucode[pc * 4], m_ucode[pc * 4 + 1], m_ucode[pc * 4 + 2], m_ucode[pc * 4 + 3]};
			if (!instruction(in, body, out.error)) return out;
			if (in.end()) break;
		}
		std::ostringstream g;
		g << "#version 450\n" << PUSH_BLOCK;
		for (u32 i = 0; i < 16; i++)
			if (m_inputs & (1u << i)) g << "layout(location = " << i << ") in vec4 in" << i << ";\n";
		g << "layout(std140, set = 0, binding = 0) uniform VertexConstants { vec4 vc[468]; };\n";
		for (u32 i = 1; i < 16; i++) g << "layout(location = " << i << ") out vec4 v" << i << ";\n";
		g << "void main() {\n"
		     "\tvec4 r[32];\n\tfor (int i = 0; i < 32; i++) r[i] = vec4(0.0);\n"
		     "\tvec4 o[16];\n\tfor (int i = 0; i < 16; i++) o[i] = vec4(0.0, 0.0, 0.0, 1.0);\n"
		     "\tvec4 cc[2] = vec4[2](vec4(0.0), vec4(0.0));\n"
		     "\tivec4 a[2] = ivec4[2](ivec4(0), ivec4(0));\n"
		     "\tvec4 tv, ts;\n";
		g << body.str();
		// RSX applies the viewport transform (ndc * scale + offset) in hardware.
		// Fold it into clip space against the logical surface size, kept linear in w.
		// With a bottom-left window origin, window y counts up from surface.w (the window height).
		g << "\tvec4 p = o[0];\n"
		     "\tvec3 win = p.xyz * pc.vp_scale.xyz + pc.vp_offset.xyz * p.w;\n"
		     "\tfloat x = 2.0 * win.x / pc.surface.x - p.w;\n"
		     "\tfloat wy = (pc.surface.z > 0.5) ? pc.surface.w * p.w - win.y : win.y;\n"
		     "\tfloat y = 2.0 * wy / pc.surface.y - p.w;\n"
		     "\tgl_Position = vec4(x, y, win.z, p.w);\n";
		for (u32 i = 1; i < 16; i++) g << "\tv" << i << " = o[" << i << "];\n";
		g << "}\n";
		out.glsl = g.str();
		out.inputs_used = m_inputs;
		out.ok = true;
		return out;
	}

private:
	static constexpr u32 VP_INSTRUCTION_LIMIT = 512;
	const u32* m_ucode;
	u32 m_start;
	u32 m_inputs = 0;

	std::string source(const VPInsn& in, int n) {
		u32 s = in.src(n);
		u32 type = bits(s, 0, 2);
		std::string base;
		switch (type) {
		case 1: base = "r[" + std::to_string(bits(s, 2, 6)) + "]"; break;
		case 2:
			m_inputs |= 1u << in.input_src();
			base = "in" + std::to_string(in.input_src());
			break;
		case 3:
			if (in.index_const())
				base = "vc[clamp(a[" + std::to_string(in.addr_reg()) + "]." + SWZ[in.addr_swz()] + " + " +
				       std::to_string(in.const_src()) + ", 0, 467)]";
			else if (in.const_src() < 468)
				base = "vc[" + std::to_string(in.const_src()) + "]";
			else
				base = "vec4(0.0)";
			break;
		default: base = "vec4(0.0)"; break;
		}
		std::string swz = {SWZ[bits(s, 14, 2)], SWZ[bits(s, 12, 2)], SWZ[bits(s, 10, 2)], SWZ[bits(s, 8, 2)]};
		if (swz != "xyzw") base += "." + swz;
		if (in.src_abs(n)) base = "abs(" + base + ")";
		if (bits(s, 16, 1)) base = "(-" + base + ")";
		return base;
	}

	std::string vec_expr(const VPInsn& in, u32 op, std::string& err) {
		auto s0 = [&] { return source(in, 0); };
		auto s1 = [&] { return source(in, 1); };
		auto s2 = [&] { return source(in, 2); };
		switch (op) {
		case V_MOV: return s0();
		case V_MUL: return s0() + " * " + s1();
		case V_ADD: return s0() + " + " + s2();
		case V_MAD: return s0() + " * " + s1() + " + " + s2();
		case V_DP3: return "vec4(dot(" + s0() + ".xyz, " + s1() + ".xyz))";
		case V_DPH: return "vec4(dot(vec4(" + s0() + ".xyz, 1.0), " + s1() + "))";
		case V_DP4: return "vec4(dot(" + s0() + ", " + s1() + "))";
		case V_DST: {
			std::string a = s0(), b = s1();
			return "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
		}
		case V_MIN: return "min(" + s0() + ", " + s1() + ")";
		case V_MAX: return "max(" + s0() + ", " + s1() + ")";
		case V_SLT: return "vec4(lessThan(" + s0() + ", " + s1() + "))";
		case V_SGE: return "vec4(greaterThanEqual(" + s0() + ", " + s1() + "))";
		case V_SEQ: return "vec4(equal(" + s0() + ", " + s1() + "))";
		case V_SGT: return "vec4(greaterThan(" + s0() + ", " + s1() + "))";
		case V_SLE: return "vec4(lessThanEqual(" + s0() + ", " + s1() + "))";
		case V_SNE: return "vec4(notEqual(" + s0() + ", " + s1() + "))";
		case V_SFL: return "vec4(0.0)";
		case V_STR: return "vec4(1.0)";
		case V_FRC: return "fract(" + s0() + ")";
		case V_FLR: return "floor(" + s0() + ")";
		case V_SSG: return "sign(" + s0() + ")";
		case V_ARL: return "floor(" + s0() + ")";
		case V_TXL: return "vec4(0.0) /* vertex texture fetch not supported yet */";
		default: err = "unknown vector opcode " + std::to_string(op); return "";
		}
	}

	std::string sca_expr(const VPInsn& in, u32 op, std::string& err) {
		std::string s = source(in, 2);
		switch (op) {
		case S_MOV: return s;
		case S_RCP: return "vec4(1.0) / " + s;
		case S_RCC: return "clamp(vec4(1.0) / " + s + ", 5.42101e-20, 1.884467e19)";
		case S_RSQ: return "vec4(inversesqrt(max(abs(" + s + ".x), 1e-30)))";
		case S_EXP: return "vec4(exp2(floor(" + s + ".x)), fract(" + s + ".x), exp2(" + s + ".x), 1.0)";
		case S_LOG: return "vec4(floor(log2(abs(" + s + ".x))), abs(" + s + ".x) / exp2(floor(log2(abs(" + s +
		                   ".x)))), log2(abs(" + s + ".x)), 1.0)";
		case S_LIT: return "vec4(1.0, max(" + s + ".x, 0.0), (" + s + ".x > 0.0) ? exp2(clamp(" + s +
		                   ".w, -127.9961, 127.9961) * log2(max(" + s + ".y, 1e-30))) : 0.0, 1.0)";
		case S_LG2: return "vec4(log2(max(abs(" + s + ".x), 1e-30)))";
		case S_EX2: return "vec4(exp2(" + s + ".x))";
		case S_SIN: return "vec4(sin(" + s + ".x))";
		case S_COS: return "vec4(cos(" + s + ".x))";
		default: err = "unsupported scalar opcode " + std::to_string(op) + " (flow control)"; return "";
		}
	}

	// Condition per component from the condition register, or "" when unconditional.
	std::string condition(const VPInsn& in) {
		if (!in.cond_test() || in.cond() == 7) return "";
		static const char* cmp[] = {"", "lessThan", "equal", "lessThanEqual", "greaterThan", "notEqual",
		                            "greaterThanEqual"};
		std::string swz = {SWZ[in.cond_swz(0)], SWZ[in.cond_swz(1)], SWZ[in.cond_swz(2)], SWZ[in.cond_swz(3)]};
		return std::string(cmp[in.cond()]) + "(cc[" + std::to_string(in.cond_reg()) + "]." + swz + ", vec4(0.0))";
	}

	static void write(std::ostringstream& b, const std::string& dst, const std::string& val, u32 mask,
	                  const std::string& cond) {
		for (int c = 0; c < 4; c++) {
			if (!(mask & (1u << c))) continue;
			b << "\t";
			if (!cond.empty()) b << "if (c[" << c << "]) ";
			b << dst << "." << SWZ[c] << " = " << val << "." << SWZ[c] << ";\n";
		}
	}

	bool instruction(const VPInsn& in, std::ostringstream& b, std::string& err) {
		u32 vop = in.vec_op(), sop = in.sca_op();
		bool has_vec = vop != V_NOP, has_sca = sop != S_NOP;
		if (in.index_input()) {
			err = "indexed vertex input not supported";
			return false;
		}
		if (in.cond_test() && in.cond() == 0) return true;   // condition "never": no effect
		if (has_vec) {
			std::string e = vec_expr(in, vop, err);
			if (e.empty()) return false;
			b << "\ttv = " << e << ";\n";
		}
		if (has_sca) {
			std::string e = sca_expr(in, sop, err);
			if (e.empty()) return false;
			b << "\tts = " << e << ";\n";
		}
		std::string cond = condition(in);
		if (!cond.empty()) b << "\t{ bvec4 c = " << cond << ";\n";
		if (has_vec) {
			std::string v = in.saturate() ? "clamp(tv, 0.0, 1.0)" : "tv";
			u32 mask = in.vec_mask();
			if (vop == V_ARL) {
				for (int c = 0; c < 4; c++)
					if (mask & (1u << c))
						b << "\t" << (cond.empty() ? "" : "if (c[" + std::to_string(c) + "]) ") << "a["
						  << (in.dst_tmp() & 1) << "]." << SWZ[c] << " = int(tv." << SWZ[c] << ");\n";
			} else {
				if (in.vec_result() && in.dst() != 0x1F) write(b, "o[" + std::to_string(in.dst() & 15) + "]", v, mask, cond);
				if (in.dst_tmp() != 0x3F) write(b, "r[" + std::to_string(in.dst_tmp() & 31) + "]", v, mask, cond);
				if (in.cond_update()) write(b, "cc[" + std::to_string(in.cond_reg()) + "]", "tv", mask, cond);
			}
		}
		if (has_sca) {
			std::string v = in.saturate() ? "clamp(ts, 0.0, 1.0)" : "ts";
			u32 mask = in.sca_mask();
			if (!in.vec_result() && in.dst() != 0x1F) write(b, "o[" + std::to_string(in.dst() & 15) + "]", v, mask, cond);
			if (in.sca_dst_tmp() != 0x3F) write(b, "r[" + std::to_string(in.sca_dst_tmp() & 31) + "]", v, mask, cond);
			if (in.cond_update() && !has_vec) write(b, "cc[" + std::to_string(in.cond_reg()) + "]", "ts", mask, cond);
		}
		if (!cond.empty()) b << "\t}\n";
		return true;
	}
};

} // namespace

VertexProgram translate_vertex(const u32* ucode, u32 start) {
	return VPTranslator(ucode, start).run();
}

} // namespace rsx::shader
