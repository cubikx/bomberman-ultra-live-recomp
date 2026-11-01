// RSX fragment program microcode -> GLSL.
//
// Instructions are 16 bytes. An instruction with a constant operand is followed
// by the 16-byte constant itself; games rewrite those in place between draws,
// so constants become fc[] uniforms refreshed from guest memory every draw.
#include <sstream>

#include "shader.h"

namespace rsx::shader {
namespace {

u32 bits(u32 w, u32 lo, u32 n) { return (w >> lo) & ((1u << n) - 1); }

struct FPInsn {
	u32 dst, src0, src1, src2;
	bool end() const { return bits(dst, 0, 1); }
	u32 dest_reg() const { return bits(dst, 1, 6); }
	bool dest_fp16() const { return bits(dst, 7, 1); }
	bool set_cond() const { return bits(dst, 8, 1); }
	u32 mask() const { return bits(dst, 9, 4); }   // x=bit0 .. w=bit3
	u32 input_reg() const { return bits(dst, 13, 4); }
	u32 tex_num() const { return bits(dst, 17, 4); }
	bool exp_tex() const { return bits(dst, 21, 1); }
	u32 prec() const { return bits(dst, 22, 2); }
	u32 opcode() const { return bits(dst, 24, 6) | (bits(src1, 31, 1) << 6); }
	bool no_dest() const { return bits(dst, 30, 1); }
	bool saturate() const { return bits(dst, 31, 1); }
	// condition (in src0)
	bool exec_lt() const { return bits(src0, 18, 1); }
	bool exec_eq() const { return bits(src0, 19, 1); }
	bool exec_gt() const { return bits(src0, 20, 1); }
	u32 cond_swz(int c) const { return bits(src0, 21 + 2 * c, 2); }
	u32 cond_mod_reg() const { return bits(src0, 30, 1); }
	u32 cond_reg() const { return bits(src0, 31, 1); }
	u32 scale() const { return bits(src1, 28, 3); }
	u32 src(int n) const { return n == 0 ? src0 : n == 1 ? src1 : src2; }
	bool src_abs(int n) const { return n == 0 ? bits(src0, 29, 1) : bits(src(n), 18, 1); }
	bool uses_constant() const {
		return bits(src0, 0, 2) == 2 || bits(src1, 0, 2) == 2 || bits(src2, 0, 2) == 2;
	}
};

enum Op {
	NOP, MOV, MUL, ADD, MAD, DP3, DP4, DST, MIN, MAX, SLT, SGE, SLE, SGT, SNE, SEQ, FRC, FLR, KIL, PK4, UP4,
	DDX, DDY, TEX, TXP, TXD, RCP, RSQ, EX2, LG2, LIT, LRP, STR, SFL, COS, SIN, PK2, UP2, POW, PKB, UPB, PK16,
	UP16, BEM, PKG, UPG, DP2A, TXL, TXB = 49, TEXBEM = 51, TXPBEM, BEMLUM, REFL, TIMESWTEX, DP2, NRM, DIV,
	DIVSQ, LIF, FENCT, FENCB, BRK = 64, CAL, IFE, LOOP, REP, RET
};

bool is_texture_op(u32 op) {
	return op == TEX || op == TXP || op == TXD || op == TXL || op == TXB || op == TEXBEM || op == TXPBEM;
}

const char SWZ[] = "xyzw";
constexpr u32 MAX_INSTRUCTIONS = 4096;
constexpr u32 SHADER_CONTROL_DEPTH_EXPORT = 0xE, SHADER_CONTROL_32_BIT_EXPORTS = 0x40;

FPInsn fetch(u32 ea) { return {fp_word(ea), fp_word(ea + 4), fp_word(ea + 8), fp_word(ea + 12)}; }

class FPTranslator {
public:
	FPTranslator(u32 ea, u32 control, const TexDim* dims) : m_ea(ea), m_control(control), m_dims(dims) {}

	FragmentProgram run() {
		FragmentProgram out;
		std::ostringstream body;
		u32 addr = m_ea;
		for (u32 n = 0; n < MAX_INSTRUCTIONS; n++) {
			FPInsn in = fetch(addr);
			m_const_index = -1;
			if (in.uses_constant()) {
				u32 off = addr + 16 - m_ea;
				m_const_index = int(out.constant_offsets.size());
				out.constant_offsets.push_back(off);
				if (out.constant_offsets.size() > MAX_FRAGMENT_CONSTANTS) {
					out.error = "too many fragment constants";
					return out;
				}
			}
			if (!instruction(in, body, out.error)) return out;
			if (in.end()) break;
			addr += in.uses_constant() ? 32 : 16;
		}

		std::ostringstream g;
		g << "#version 450\n" << PUSH_BLOCK;
		for (u32 i = 1; i < 16; i++)
			if (m_varyings & (1u << i)) g << "layout(location = " << i << ") in vec4 v" << i << ";\n";
		g << "layout(std140, set = 0, binding = 1) uniform FragmentConstants { vec4 fc[" << MAX_FRAGMENT_CONSTANTS
		  << "]; };\n";
		for (u32 t = 0; t < MAX_TEXTURE_UNITS; t++) {
			if (!(m_textures & (1u << t))) continue;
			static const char* types[] = {"sampler2D", "samplerCube", "sampler3D"};
			g << "layout(set = 0, binding = " << (2 + t) << ") uniform " << types[int(m_dims[t])] << " t" << t << ";\n";
		}
		g << "layout(location = 0) out vec4 out_color;\n"
		     "void main() {\n"
		     "\tvec4 r[64];\n\tfor (int i = 0; i < 64; i++) r[i] = vec4(0.0);\n"
		     "\tvec4 h[64];\n\tfor (int i = 0; i < 64; i++) h[i] = vec4(0.0);\n"
		     "\tvec4 cc[2] = vec4[2](vec4(0.0), vec4(0.0));\n"
		     "\tvec4 t;\n";
		g << body.str();
		g << "\tout_color = " << ((m_control & SHADER_CONTROL_32_BIT_EXPORTS) ? "r[0]" : "h[0]") << ";\n";
		if (m_control & SHADER_CONTROL_DEPTH_EXPORT) g << "\tgl_FragDepth = r[1].z;\n";
		// alpha test (fixed function on the RSX)
		g << "\tif (pc.alpha_test.z > 0.5) {\n"
		     "\t\tfloat a = out_color.a, ref = pc.alpha_test.y;\n"
		     "\t\tint f = int(pc.alpha_test.x);\n"
		     "\t\tbool pass = f == 7 || (f == 1 && a < ref) || (f == 2 && a == ref) || (f == 3 && a <= ref) ||\n"
		     "\t\t            (f == 4 && a > ref) || (f == 5 && a != ref) || (f == 6 && a >= ref);\n"
		     "\t\tif (!pass) discard;\n"
		     "\t}\n"
		     "}\n";
		out.glsl = g.str();
		out.textures_used = m_textures;
		out.ok = true;
		return out;
	}

private:
	u32 m_ea, m_control;
	const TexDim* m_dims;
	int m_const_index = -1;
	u32 m_varyings = 0, m_textures = 0;

	std::string input(u32 reg, std::string& err) {
		auto var = [&](u32 v) {
			m_varyings |= 1u << v;
			return "v" + std::to_string(v);
		};
		switch (reg) {
		case 0:   // WPOS, in guest pixels (the target may be rendered at a multiple of its size)
			return "vec4(gl_FragCoord.x / pc.alpha_test.w, (pc.surface.z > 0.5) ? pc.surface.w - gl_FragCoord.y / "
			       "pc.alpha_test.w : gl_FragCoord.y / pc.alpha_test.w, gl_FragCoord.z, gl_FragCoord.w)";
		case 1: return "clamp(" + var(1) + ", 0.0, 1.0)";   // COL0
		case 2: return "clamp(" + var(2) + ", 0.0, 1.0)";   // COL1
		case 3: return var(5) + ".xxxx";                    // FOGC
		case 12: return var(15);                            // TEX8
		case 13: return var(6);                             // TEX9
		case 14: return "vec4(gl_FrontFacing ? 1.0 : -1.0)"; // SSA
		default:
			if (reg >= 4 && reg <= 11) return var(7 + reg - 4);   // TEX0..TEX7
			err = "unknown fragment input " + std::to_string(reg);
			return "vec4(0.0)";
		}
	}

	std::string source(const FPInsn& in, int n, std::string& err) {
		u32 s = in.src(n);
		std::string base;
		switch (bits(s, 0, 2)) {
		case 0: base = std::string(bits(s, 8, 1) ? "h[" : "r[") + std::to_string(bits(s, 2, 6)) + "]"; break;
		case 1:
			if (n == 2 && bits(in.src2, 30, 1)) err = "indexed fragment input not supported";
			base = input(in.input_reg(), err);
			break;
		case 2: base = "fc[" + std::to_string(m_const_index) + "]"; break;
		default: base = "vec4(0.0)"; break;
		}
		std::string swz = {SWZ[bits(s, 9, 2)], SWZ[bits(s, 11, 2)], SWZ[bits(s, 13, 2)], SWZ[bits(s, 15, 2)]};
		if (swz != "xyzw") base += "." + swz;
		if (in.src_abs(n)) base = "abs(" + base + ")";
		if (bits(s, 17, 1)) base = "(-" + base + ")";
		return base;
	}

	std::string sampler(const FPInsn& in) {
		m_textures |= 1u << in.tex_num();
		return "t" + std::to_string(in.tex_num());
	}

	std::string coords(const FPInsn& in, const std::string& s) {
		switch (m_dims[in.tex_num()]) {
		case TexDim::cube:
		case TexDim::tex3d: return s + ".xyz";
		default: return s + ".xy";
		}
	}

	std::string expr(const FPInsn& in, u32 op, std::string& err) {
		auto s = [&](int n) { return source(in, n, err); };
		switch (op) {
		case MOV: return s(0);
		case MUL: return s(0) + " * " + s(1);
		case ADD: return s(0) + " + " + s(1);
		case MAD: return s(0) + " * " + s(1) + " + " + s(2);
		case DP3: return "vec4(dot(" + s(0) + ".xyz, " + s(1) + ".xyz))";
		case DP4: return "vec4(dot(" + s(0) + ", " + s(1) + "))";
		case DP2: return "vec4(dot(" + s(0) + ".xy, " + s(1) + ".xy))";
		case DP2A: return "vec4(dot(" + s(0) + ".xy, " + s(1) + ".xy) + " + s(2) + ".z)";
		case DST: {
			std::string a = s(0), b = s(1);
			return "vec4(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
		}
		case MIN: return "min(" + s(0) + ", " + s(1) + ")";
		case MAX: return "max(" + s(0) + ", " + s(1) + ")";
		case SLT: return "vec4(lessThan(" + s(0) + ", " + s(1) + "))";
		case SGE: return "vec4(greaterThanEqual(" + s(0) + ", " + s(1) + "))";
		case SLE: return "vec4(lessThanEqual(" + s(0) + ", " + s(1) + "))";
		case SGT: return "vec4(greaterThan(" + s(0) + ", " + s(1) + "))";
		case SNE: return "vec4(notEqual(" + s(0) + ", " + s(1) + "))";
		case SEQ: return "vec4(equal(" + s(0) + ", " + s(1) + "))";
		case STR: return "vec4(1.0)";
		case SFL: return "vec4(0.0)";
		case FRC: return "fract(" + s(0) + ")";
		case FLR: return "floor(" + s(0) + ")";
		case DDX: return "dFdx(" + s(0) + ")";
		case DDY: return "dFdy(" + s(0) + ")";
		case RCP: return "vec4(1.0 / " + s(0) + ".x)";
		case RSQ: return "vec4(inversesqrt(abs(" + s(0) + ".x)))";
		case EX2: return "vec4(exp2(" + s(0) + ".x))";
		case LG2: return "vec4(log2(" + s(0) + ".x))";
		case COS: return "vec4(cos(" + s(0) + ".x))";
		case SIN: return "vec4(sin(" + s(0) + ".x))";
		case POW: return "vec4(pow(" + s(0) + ".x, " + s(1) + ".x))";
		case DIV: return s(0) + " / " + s(1) + ".x";
		case DIVSQ: return s(0) + " * inversesqrt(" + s(1) + ".x)";
		case LRP: return "mix(" + s(2) + ", " + s(1) + ", " + s(0) + ")";
		case NRM: return "vec4(normalize(" + s(0) + ".xyz), 0.0)";
		case REFL: {
			std::string a = s(0), b = s(1);
			return "(2.0 * dot(" + a + ".xyz, " + b + ".xyz) * " + a + " - " + b + ")";
		}
		case LIT:
		case LIF: {
			std::string a = s(0);
			return "vec4(1.0, max(" + a + ".x, 0.0), (" + a + ".x > 0.0) ? exp2(" + a + ".w * log2(max(" + a +
			       ".y, 1e-30))) : 0.0, 1.0)";
		}
		case PK4: return "vec4(uintBitsToFloat(packSnorm4x8(" + s(0) + ")))";
		case UP4: return "unpackSnorm4x8(floatBitsToUint(" + s(0) + ".x))";
		case PKB:
		case PKG: return "vec4(uintBitsToFloat(packUnorm4x8(" + s(0) + ")))";
		case UPB:
		case UPG: return "unpackUnorm4x8(floatBitsToUint(" + s(0) + ".x))";
		case PK2: return "vec4(uintBitsToFloat(packHalf2x16(" + s(0) + ".xy)))";
		case UP2: return "unpackHalf2x16(floatBitsToUint(" + s(0) + ".x)).xyxy";
		case PK16: return "vec4(uintBitsToFloat(packUnorm2x16(" + s(0) + ".xy)))";
		case UP16: return "unpackUnorm2x16(floatBitsToUint(" + s(0) + ".x)).xyxy";
		case TEX:
		case TEXBEM: return "texture(" + sampler(in) + ", " + coords(in, s(0)) + ")";
		case TXP:
		case TXPBEM: {
			std::string a = s(0);
			if (m_dims[in.tex_num()] != TexDim::tex2d) return "texture(" + sampler(in) + ", " + coords(in, a) + ")";
			return "texture(" + sampler(in) + ", " + a + ".xy / " + a + ".w)";
		}
		case TXB: {
			std::string a = s(0);
			return "texture(" + sampler(in) + ", " + coords(in, a) + ", " + a + ".w)";
		}
		case TXL: return "textureLod(" + sampler(in) + ", " + coords(in, s(0)) + ", " + s(1) + ".x)";
		case TXD: return "textureGrad(" + sampler(in) + ", " + coords(in, s(0)) + ", " + coords(in, s(1)) + ", " +
		                 coords(in, s(2)) + ")";
		case FENCT:
		case FENCB: return "";
		default:
			err = "unsupported fragment opcode " + std::to_string(op);
			return "";
		}
	}

	std::string condition(const FPInsn& in) {
		bool lt = in.exec_lt(), eq = in.exec_eq(), gt = in.exec_gt();
		if (lt && eq && gt) return "";
		const char* cmp = lt && eq ? "lessThanEqual" : gt && eq ? "greaterThanEqual" : lt && gt ? "notEqual"
		                : lt ? "lessThan" : gt ? "greaterThan" : "equal";
		std::string swz = {SWZ[in.cond_swz(0)], SWZ[in.cond_swz(1)], SWZ[in.cond_swz(2)], SWZ[in.cond_swz(3)]};
		return std::string(cmp) + "(cc[" + std::to_string(in.cond_reg()) + "]." + swz + ", vec4(0.0))";
	}

	bool instruction(const FPInsn& in, std::ostringstream& b, std::string& err) {
		u32 op = in.opcode();
		if (op == NOP || op == FENCT || op == FENCB) return true;
		if (op >= BRK) {
			err = "fragment flow control (opcode " + std::to_string(op) + ") not supported yet";
			return false;
		}
		bool never = !in.exec_lt() && !in.exec_eq() && !in.exec_gt();
		if (never) return true;
		std::string cond = condition(in);

		if (op == KIL) {
			b << "\tif (" << (cond.empty() ? "true" : "any(" + cond + ")") << ") discard;\n";
			return true;
		}
		std::string e = expr(in, op, err);
		if (!err.empty()) return false;
		if (e.empty()) return true;

		std::string v = "(" + e + ")";
		if (is_texture_op(op) && in.exp_tex()) v = "(" + v + " * 2.0 - 1.0)";
		static const char* scale[] = {"", " * 2.0", " * 4.0", " * 8.0", "", " / 2.0", " / 4.0", " / 8.0"};
		if (in.scale()) v = "(" + v + scale[in.scale()] + ")";
		if (in.saturate()) v = "clamp(" + v + ", 0.0, 1.0)";
		else if (in.prec() == 2) v = "clamp(" + v + ", -2.0, 2.0)";   // fx12
		else if (in.prec() == 3) v = "clamp(" + v + ", -1.0, 1.0)";   // fx9
		b << "\tt = " << v << ";\n";

		u32 mask = in.mask();
		if (!cond.empty()) b << "\t{ bvec4 c = " << cond << ";\n";
		auto write = [&](const std::string& dst) {
			for (int c = 0; c < 4; c++) {
				if (!(mask & (1u << c))) continue;
				b << "\t";
				if (!cond.empty()) b << "if (c[" << c << "]) ";
				b << dst << "." << SWZ[c] << " = t." << SWZ[c] << ";\n";
			}
		};
		if (!in.no_dest()) write(std::string(in.dest_fp16() ? "h[" : "r[") + std::to_string(in.dest_reg()) + "]");
		if (in.set_cond()) write("cc[" + std::to_string(in.cond_mod_reg()) + "]");
		if (!cond.empty()) b << "\t}\n";
		return true;
	}
};

} // namespace

FragmentProgram translate_fragment(u32 ea, u32 shader_control, const TexDim* tex_dims) {
	return FPTranslator(ea, shader_control, tex_dims).run();
}

FragmentKey fragment_key(u32 ea) {
	FragmentKey k;
	u64 h = 0xcbf29ce484222325ull;
	auto mix = [&](u32 v) {
		for (int i = 0; i < 4; i++) {
			h ^= (v >> (8 * i)) & 0xFF;
			h *= 0x100000001b3ull;
		}
	};
	u32 addr = ea;
	for (u32 n = 0; n < MAX_INSTRUCTIONS; n++) {
		FPInsn in = fetch(addr);
		mix(in.dst);
		mix(in.src0);
		mix(in.src1);
		mix(in.src2);
		if (is_texture_op(in.opcode())) k.textures_used |= 1u << in.tex_num();
		addr += in.uses_constant() ? 32 : 16;
		if (in.end()) break;
	}
	k.hash = h;
	k.size = addr - ea;
	return k;
}

} // namespace rsx::shader
