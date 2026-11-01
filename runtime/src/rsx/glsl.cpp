// GLSL -> SPIR-V with glslang (shaders are generated at runtime from RSX microcode).
#include "glsl.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include <mutex>
#include <sstream>

#include "runtime/kernel.h"

namespace rsx::glsl {

static void log_numbered(const std::string& src) {
	std::istringstream in(src);
	std::string line, all;
	for (int n = 1; std::getline(in, line); n++) all += std::to_string(n) + ": " + line + "\n";
	LOG_ERROR("%s", all.c_str());
}

std::vector<uint32_t> compile(Stage stage, const std::string& source) {
	static std::once_flag once;
	std::call_once(once, [] { glslang::InitializeProcess(); });

	EShLanguage lang = stage == Stage::vertex ? EShLangVertex : EShLangFragment;
	glslang::TShader shader(lang);
	const char* text = source.c_str();
	shader.setStrings(&text, 1);
	shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
	shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
	shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
	if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault)) {
		LOG_ERROR("glsl: %s shader failed to compile:\n%s", stage == Stage::vertex ? "vertex" : "fragment",
		          shader.getInfoLog());
		log_numbered(source);
		return {};
	}
	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(EShMsgDefault)) {
		LOG_ERROR("glsl: link failed:\n%s", program.getInfoLog());
		return {};
	}
	std::vector<uint32_t> spirv;
	glslang::GlslangToSpv(*program.getIntermediate(lang), spirv);
	return spirv;
}

} // namespace rsx::glsl
