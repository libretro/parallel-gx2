#pragma once
#include "util/helpers/Serializer.h"

namespace Latte
{
	// the dictionary the entries are compressed with; before the first
	// (de)serialize, once
	void InitShaderSerializer();
	void SerializeShaderProgram(void* shaderProg, uint32 size, MemStreamWriter& memWriter);
	bool DeserializeShaderProgram(std::vector<uint8>& progData, MemStreamReader& memReader);
};