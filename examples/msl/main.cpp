#include <NZSL/MslWriter.hpp>
#include <NZSL/Parser.hpp>
#include <iostream>

int main()
{
	auto shader = nzsl::ParseFromFile("shader.nzsl");
	nzsl::MslWriter mslWriter;
	auto mslShader = mslWriter.Generate(*shader);
	std::cout << mslShader << std::endl;
	return 0;
}
