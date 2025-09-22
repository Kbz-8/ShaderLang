#include <NZSL/MslWriter.hpp>
#include <NZSL/Parser.hpp>
#include <iostream>
#include <fstream>

int main()
{
	auto shader = nzsl::ParseFromFile("shader.nzsl");
	nzsl::MslWriter mslWriter;
	auto mslShader = mslWriter.Generate(*shader);
	std::cout << mslShader << std::endl;
	std::ofstream file("/tmp/test/output.metal");
	file << mslShader << std::endl;
	return 0;
}
