// Copyright (C) 2025 kbz_8 (contact@kbz8.me)
// This file is part of the "Nazara Shading Language" project
// For conditions of distribution and use, see copyright notice in Config.hpp

#include <NZSL/MslWriter.hpp>
#include <NazaraUtils/Algorithm.hpp>
#include <NazaraUtils/CallOnExit.hpp>
#include <NZSL/Enums.hpp>
#include <NZSL/Lexer.hpp>
#include <NZSL/Parser.hpp>
#include <NZSL/Ast/RecursiveVisitor.hpp>
#include <NZSL/Ast/Utils.hpp>
#include <NZSL/Lang/Constants.hpp>
#include <NZSL/Lang/LangData.hpp>
#include <NZSL/Lang/Version.hpp>
#include <NZSL/Ast/Transformations/AliasTransformer.hpp>
#include <NZSL/Ast/Transformations/BindingResolverTransformer.hpp>
#include <NZSL/Ast/Transformations/BranchSplitterTransformer.hpp>
#include <NZSL/Ast/Transformations/ConstantPropagationTransformer.hpp>
#include <NZSL/Ast/Transformations/ConstantRemovalTransformer.hpp>
#include <NZSL/Ast/Transformations/EliminateUnusedTransformer.hpp>
#include <NZSL/Ast/Transformations/ForToWhileTransformer.hpp>
#include <NZSL/Ast/Transformations/IdentifierTransformer.hpp>
#include <NZSL/Ast/Transformations/LoopUnrollTransformer.hpp>
#include <NZSL/Ast/Transformations/LiteralTransformer.hpp>
#include <NZSL/Ast/Transformations/MatrixTransformer.hpp>
#include <NZSL/Ast/Transformations/ResolveTransformer.hpp>
#include <NZSL/Ast/Transformations/StructAssignmentTransformer.hpp>
#include <NZSL/Ast/Transformations/SwizzleTransformer.hpp>
#include <NZSL/Ast/Transformations/ValidationTransformer.hpp>
#include <fmt/format.h>
#include <frozen/unordered_map.h>
#include <frozen/unordered_set.h>
#include <tsl/ordered_set.h>
#include <cassert>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace nzsl
{
	constexpr std::string_view s_mslNamespace = "metal::";
	constexpr std::string_view s_mslExternalStructName = "_nzslExternals";

	constexpr auto s_mslBuiltinMapping = frozen::make_unordered_map<Ast::BuiltinEntry, std::string_view>({
		{ Ast::BuiltinEntry::BaseInstance,            { "base_instance" } },
		{ Ast::BuiltinEntry::BaseVertex,              { "base_vertex" } },
		{ Ast::BuiltinEntry::DrawIndex,               {} },
		{ Ast::BuiltinEntry::FragCoord,               { "position" } },
		{ Ast::BuiltinEntry::FragDepth,               { "depth(any)" } },
		{ Ast::BuiltinEntry::GlocalInvocationIndices, { "thread_position_in_grid" } },
		{ Ast::BuiltinEntry::InstanceIndex,           { "instance_id" } },
		{ Ast::BuiltinEntry::LocalInvocationIndex,    { "thread_index_in_threadgroup" } },
		{ Ast::BuiltinEntry::LocalInvocationIndices,  { "thread_position_in_threadgroup" } },
		{ Ast::BuiltinEntry::VertexIndex,             { "vertex_id" } },
		{ Ast::BuiltinEntry::VertexPosition,          { "position" } },
		{ Ast::BuiltinEntry::WorkgroupCount,          { "threadgroups_per_grid" } },
		{ Ast::BuiltinEntry::WorkgroupIndices,        { "threadgroup_position_in_grid" } },
	});

	struct MslWriter::PreVisitor : Ast::RecursiveVisitor
	{
		PreVisitor(MslWriter& writer) :
		m_writer(writer)
		{
		}

		void Visit(Ast::DeclareFunctionStatement& node) override
		{
			if (node.funcIndex)
				m_writer.RegisterFunction(*node.funcIndex, node.name);

			// Speed up by not visiting function statements, we only need to extract function data
		}

		MslWriter& m_writer;
	};

	struct MslWriter::AutoBindingAttribute
	{
		const Ast::ExpressionValue<bool>& autoBinding;

		bool HasValue() const { return autoBinding.HasValue(); }
	};

	struct MslWriter::AuthorAttribute
	{
		const std::string& author;

		bool HasValue() const { return !author.empty(); }
	};

	struct MslWriter::BindingAttribute
	{
		const Ast::ExpressionValue<std::uint32_t>& bindingIndex;

		bool HasValue() const { return bindingIndex.HasValue(); }
	};

	struct MslWriter::BuiltinAttribute
	{
		const Ast::ExpressionValue<Ast::BuiltinEntry>& builtin;

		bool HasValue() const { return builtin.HasValue(); }
	};

	struct MslWriter::CondAttribute
	{
		const Ast::ExpressionValue<bool>& cond;

		bool HasValue() const { return cond.HasValue(); }
	};

	struct MslWriter::DepthWriteAttribute
	{
		const Ast::ExpressionValue<Ast::DepthWriteMode>& writeMode;

		bool HasValue() const { return writeMode.HasValue(); }
	};

	struct MslWriter::DescriptionAttribute
	{
		const std::string& description;

		bool HasValue() const { return !description.empty(); }
	};

	struct MslWriter::EarlyFragmentTestsAttribute
	{
		const Ast::ExpressionValue<bool>& earlyFragmentTests;

		bool HasValue() const { return earlyFragmentTests.HasValue(); }
	};

	struct MslWriter::EntryAttribute
	{
		const Ast::ExpressionValue<ShaderStageType>& stageType;

		bool HasValue() const { return stageType.HasValue(); }
	};

	struct MslWriter::FeatureAttribute
	{
		Ast::ModuleFeature featureAttribute;

		bool HasValue() const { return true; }
	};

	struct MslWriter::InterpAttribute
	{
		const Ast::ExpressionValue<Ast::InterpolationQualifier>& interpQualifier;

		bool HasValue() const { return interpQualifier.HasValue(); }
	};

	struct MslWriter::LangVersionAttribute
	{
		std::uint32_t version;

		bool HasValue() const { return true; }
	};

	struct MslWriter::LayoutAttribute
	{
		const Ast::ExpressionValue<Ast::MemoryLayout>& layout;

		bool HasValue() const { return layout.HasValue(); }
	};

	struct MslWriter::LicenseAttribute
	{
		const std::string& license;

		bool HasValue() const { return !license.empty(); }
	};

	struct MslWriter::LocationAttribute
	{
		const Ast::ExpressionValue<std::uint32_t>& locationIndex;

		bool HasValue() const { return locationIndex.HasValue(); }
	};

	struct MslWriter::SetAttribute
	{
		const Ast::ExpressionValue<std::uint32_t>& setIndex;

		bool HasValue() const { return setIndex.HasValue(); }
	};

	struct MslWriter::TagAttribute
	{
		const std::string& tag;

		bool HasValue() const { return !tag.empty(); }
	};

	struct MslWriter::UnrollAttribute
	{
		const Ast::ExpressionValue<Ast::LoopUnroll>& unroll;

		bool HasValue() const { return unroll.HasValue(); }
	};

	struct MslWriter::WorkgroupAttribute
	{
		const Ast::ExpressionValue<Vector3u32>& workgroup;

		bool HasValue() const { return workgroup.HasValue(); }
	};

	struct MslWriter::State
	{
		struct Identifier
		{
			std::optional<std::size_t> externalBlockIndex;
			std::size_t moduleIndex;
			std::string name;
		};

		struct StructData : Identifier
		{
			const Ast::StructDescription* desc;
		};

		std::optional<std::size_t> currentExternalBlockIndex;
		std::size_t currentModuleIndex;
		std::stringstream stream;
		std::unordered_map<std::size_t, Identifier> aliases;
		std::unordered_map<std::size_t, Identifier> constants;
		std::unordered_map<std::size_t, Identifier> functions;
		std::unordered_map<std::size_t, Identifier> modules;
		std::unordered_map<std::size_t, StructData> structs;
		std::unordered_map<std::size_t, Identifier> variables;
		std::vector<std::string> moduleNames;
		const Ast::Module* currentModule;
		bool enforceNonDefaultTypes = false;
		bool isInEntryPoint = false;
		int streamEmptyLine = 1;
		unsigned int indentLevel = 0;
		std::uint16_t externalBuffersCount = 0;
		std::uint16_t externalTexturesCount = 0;
		bool hasExternalStructDeclared = false;
	};

	std::string MslWriter::Generate(Ast::Module& module, const BackendParameters& parameters)
	{
		State state;
		m_currentState = &state;
		NAZARA_DEFER({ m_currentState = nullptr; });

		if (parameters.backendPasses)
		{
			Ast::TransformerExecutor executor;
			if (parameters.backendPasses.Test(BackendPass::Resolve))
			{
				executor.AddPass<Ast::ResolveTransformer>([&](Ast::ResolveTransformer::Options& opt)
				{
					opt.moduleResolver = parameters.shaderModuleResolver;
				});
			}

			if (parameters.backendPasses.Test(BackendPass::TargetRequired))
				RegisterPasses(executor);

			if (parameters.backendPasses.Test(BackendPass::Optimize))
				executor.AddPass<Ast::ConstantPropagationTransformer>();

			if (parameters.backendPasses.Test(BackendPass::Validate))
			{
				executor.AddPass<Ast::ValidationTransformer>([](Ast::ValidationTransformer::Options& opt)
				{
					opt.allowUntyped = false;
					opt.checkIndices = true;
				});
			}

			Ast::TransformerContext context;
			context.optionValues = parameters.optionValues;

			executor.Transform(module, context);
		}

		if (parameters.backendPasses.Test(BackendPass::RemoveDeadCode))
		{
			Ast::DependencyCheckerVisitor::Config dependencyConfig;
			dependencyConfig.usedShaderStages = ShaderStageType_All;

			Ast::EliminateUnusedPass(module, dependencyConfig);
		}

		AppendHeader(module);
		AppendLine("#include <metal_stdlib>");
		AppendLine("#include <simd/simd.h>");
		AppendLine();

		// First registration pass (required to register function names)
		PreVisitor previsitor(*this);
		{
			m_currentState->currentModuleIndex = 0;
			for (const auto& importedModule : module.importedModules)
			{
				importedModule.module->rootNode->Visit(previsitor);
				m_currentState->currentModuleIndex++;
				m_currentState->moduleNames.push_back(importedModule.identifier);
			}

			m_currentState->currentModuleIndex = std::numeric_limits<std::size_t>::max();

			std::size_t moduleIndex = 0;
			for (const auto& importedModule : module.importedModules)
				RegisterModule(moduleIndex++, importedModule.identifier);

			module.rootNode->Visit(previsitor);
		}

		// Register imported modules
		m_currentState->currentModuleIndex = 0;
		for (const auto& importedModule : module.importedModules)
		{
			m_currentState->currentModule = importedModule.module.get();

			AppendModuleAttributes(*importedModule.module->metadata);
			AppendLine("module ", importedModule.identifier);
			EnterScope();
			importedModule.module->rootNode->Visit(*this);
			LeaveScope(true);

			m_currentState->currentModuleIndex++;
			m_currentState->moduleNames.push_back(importedModule.identifier);
		}

		m_currentState->currentModule = &module;
		m_currentState->currentModuleIndex = std::numeric_limits<std::size_t>::max();
		module.rootNode->Visit(*this);

		return state.stream.str();
	}

	void MslWriter::RegisterPasses(Ast::TransformerExecutor& executor)
	{
		// Metal Shading Language is based on C++ 14/17 spec and so it uses C++ keywords (with some features being removed like exceptions)
		static constexpr auto s_reservedKeywords = frozen::make_unordered_set<frozen::string>({
			"alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept", "auto", "bitand",
			"bitor", "bool", "break", "case", "char", "char16_t", "char32_t", "class", "compl", "const", "constexpr", "const_cast",
			"continue", "decltype", "default", "do", "double", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
			"friend", "if", "inline", "int", "long", "mutable", "namespace", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
			"private", "protected", "public", "reinterpret_cast", "return", "short", "signed", "sizeof", "static", "static_assert",
			"static_cast", "struct", "switch", "template", "this", "true", "typedef", "typename", "union", "unsigned", "using", "void",
			"volatile", "wchar_t", "while", "xor", "xor_eq", "device", "constant", "threadgroup", "threadgroup_imageblock", "object_data", "ray_data",
		});

		// We need two identifiers passes, the first one to rename reserved/forbidden variable names and the second one to ensure all variables name are uniques (which isn't guaranteed by the transformation passes)
		// We can't do this at once at the end because transformations passes will introduce variables prefixed by _nzsl which is forbidden in user code
		Ast::IdentifierTransformer::Options firstIdentifierPassOptions;
		firstIdentifierPassOptions.makeVariableNameUnique = false;
		firstIdentifierPassOptions.identifierSanitizer = [](std::string& identifier, Ast::IdentifierCategory /*scope*/)
		{
			using namespace std::string_view_literals;

			bool nameChanged = false;

			// Identifier can't start with _nzsl
			if (identifier.compare(0, 5, "_nzsl") == 0)
			{
				identifier.replace(0, 5, "_"sv);
				nameChanged = true;
			}

			// Identifier can't be named "main"
			if (identifier == "main")
			{
				identifier = "main0"sv;
				nameChanged = true;
			}

			return nameChanged;
		};

		Ast::IdentifierTransformer::Options secondIdentifierPassOptions;
		secondIdentifierPassOptions.makeVariableNameUnique = true;
		secondIdentifierPassOptions.identifierSanitizer = [](std::string& identifier, Ast::IdentifierCategory /*scope*/)
		{
			using namespace std::string_view_literals;

			bool nameChanged = false;
			while (s_reservedKeywords.count(frozen::string(identifier)) != 0)
			{
				identifier += '_';
				nameChanged = true;
			}

			return nameChanged;
		};

		executor.AddPass<Ast::LoopUnrollTransformer>();
		executor.AddPass<Ast::LiteralTransformer>();
		//executor.AddPass<Ast::BranchSplitterTransformer>();
		executor.AddPass<Ast::IdentifierTransformer>(firstIdentifierPassOptions);
		executor.AddPass<Ast::ForToWhileTransformer>();
		executor.AddPass<Ast::StructAssignmentTransformer>([](Ast::StructAssignmentTransformer::Options& opt)
		{
			opt.splitWrappedArrayAssignation = false;
			opt.splitWrappedStructAssignation = true;
		});
		executor.AddPass<Ast::SwizzleTransformer>([](Ast::SwizzleTransformer::Options& opt)
		{
			opt.removeScalarSwizzling = true;
			//opt.removeSwizzleAssigment = true;
		});
		executor.AddPass<Ast::MatrixTransformer>([](Ast::MatrixTransformer::Options& opt)
		{
			opt.removeMatrixBinaryAddSub = true;
			opt.removeMatrixCast = true;
		});
		executor.AddPass<Ast::BindingResolverTransformer>();
		executor.AddPass<Ast::ConstantRemovalTransformer>([](Ast::ConstantRemovalTransformer::Options& opt)
		{
			opt.removeConstArraySize = false;
			opt.removeTypeConstant = false;
		});
		executor.AddPass<Ast::AliasTransformer>();
		executor.AddPass<Ast::IdentifierTransformer>(secondIdentifierPassOptions);
	}

	void MslWriter::SetEnv(Environment environment)
	{
		m_environment = std::move(environment);
	}

	void MslWriter::Append(const Ast::AliasType& type)
	{
		AppendIdentifier(m_currentState->aliases, type.aliasIndex);
	}

	void MslWriter::Append(const Ast::ArrayType& type)
	{
		if (type.length > 0)
			Append("array[", type.InnerType(), ", ", type.length, "]");
		else
			Append("array[", type.InnerType(), "]");
	}

	void MslWriter::Append(const Ast::DynArrayType& type)
	{
		Append("dyn_array[", type.InnerType(), "]");
	}

	void MslWriter::Append(const Ast::ExpressionType& type)
	{
		std::visit([&](auto&& arg)
		{
			Append(arg);
		}, type);
	}

	void MslWriter::Append(const Ast::ExpressionValue<Ast::ExpressionType>& type)
	{
		assert(type.HasValue());
		if (type.IsResultingValue())
			Append(type.GetResultingValue());
		else
			type.GetExpression()->Visit(*this);
	}

	void MslWriter::Append(const Ast::FunctionType& /*functionType*/)
	{
		throw std::runtime_error("unexpected function type");
	}

	void MslWriter::Append(const Ast::ImplicitArrayType& /*arrayType*/)
	{
		throw std::runtime_error("unexpected ImplicitVectorType");
	}

	void MslWriter::Append(const Ast::ImplicitMatrixType& /*matrixType*/)
	{
		throw std::runtime_error("unexpected ImplicitMatrixType");
	}

	void MslWriter::Append(const Ast::ImplicitVectorType& /*;vecType*/)
	{
		throw std::runtime_error("unexpected ImplicitVectorType");
	}

	void MslWriter::Append(const Ast::IntrinsicFunctionType& /*functionType*/)
	{
		throw std::runtime_error("unexpected intrinsic function type");
	}

	void MslWriter::Append(const Ast::MatrixType& matrixType)
	{
		Append(s_mslNamespace, matrixType.type, matrixType.columnCount, 'x', matrixType.rowCount);
	}

	void MslWriter::Append(const Ast::MethodType& /*functionType*/)
	{
		throw std::runtime_error("unexpected method type");
	}

	void MslWriter::Append(const Ast::ModuleType& moduleType)
	{
		AppendIdentifier(m_currentState->modules, moduleType.moduleIndex);
	}

	void MslWriter::Append(const Ast::NamedExternalBlockType& /*namedExternalBlockType*/)
	{
		// Nothing to do
	}

	void MslWriter::Append(Ast::NoType)
	{
		return Append("(void)");
	}

	void MslWriter::Append(Ast::PrimitiveType type)
	{
		switch (type)
		{
			case Ast::PrimitiveType::Boolean:      return Append("bool");
			case Ast::PrimitiveType::Float32:      return Append("float");
			case Ast::PrimitiveType::Float64:      return Append("double");
			case Ast::PrimitiveType::Int32:        return Append("int");
			case Ast::PrimitiveType::UInt32:       return Append("uint");
			case Ast::PrimitiveType::String:       return Append("const char*"); // May be invalid
			case Ast::PrimitiveType::FloatLiteral: throw std::runtime_error("unexpected untyped float");
			case Ast::PrimitiveType::IntLiteral:   throw std::runtime_error("unexpected untyped integer");
		}
	}

	void MslWriter::Append(const Ast::PushConstantType& pushConstantType)
	{
		Append("push_constant[", pushConstantType.containedType, "]");
	}

	void MslWriter::Append(const Ast::SamplerType& samplerType)
	{
		Append(s_mslNamespace);
		if (samplerType.depth)
			Append("depth");
		else
			Append("texture");

		switch (samplerType.dim)
		{
			case ImageType::E1D:       Append("1d");      break;
			case ImageType::E1D_Array: Append("1d_array"); break;
			case ImageType::E2D:       Append("2d");      break;
			case ImageType::E2D_Array: Append("2d_array"); break;
			case ImageType::E3D:       Append("3d");      break;
			case ImageType::Cubemap:   Append("cube");    break;
		}

		Append('<', samplerType.sampledType, '>');
	}

	void MslWriter::Append(const Ast::StorageType& storageType)
	{
		Append(storageType.containedType);
	}

	void MslWriter::Append(const Ast::StructType& structType)
	{
		AppendIdentifier(m_currentState->structs, structType.structIndex);
	}

	void MslWriter::Append(const Ast::TextureType& textureType)
	{
		Append("texture");

		switch (textureType.dim)
		{
			case ImageType::E1D:       Append("1d");       break;
			case ImageType::E1D_Array: Append("1d_array"); break;
			case ImageType::E2D:       Append("2d");       break;
			case ImageType::E2D_Array: Append("2d_array"); break;
			case ImageType::E3D:       Append("3d");       break;
			case ImageType::Cubemap:   Append("_cube");    break;
		}

		Append("<", textureType.baseType, ", ");
		switch (textureType.accessPolicy)
		{
			case AccessPolicy::ReadOnly:  Append(s_mslNamespace, "access::read"); break;
			case AccessPolicy::ReadWrite: Append(s_mslNamespace, "access::read_write"); break;
			case AccessPolicy::WriteOnly: Append(s_mslNamespace, "access::write"); break;
		}

		if (textureType.format != ImageFormat::Unknown)
		{
			assert(textureType.format == ImageFormat::RGBA8); //< TODO
			Append(", rgba8");
		}
		Append("]");
	}

	void MslWriter::Append(const Ast::Type& /*type*/)
	{
		throw std::runtime_error("unexpected type?");
	}

	void MslWriter::Append(Ast::TypeConstant typeConstant)
	{
		Append(Parser::ToString(typeConstant));
	}

	void MslWriter::Append(const Ast::UniformType& uniformType)
	{
		Append(uniformType.containedType);
	}

	void MslWriter::Append(const Ast::VectorType& vecType)
	{
		Append(s_mslNamespace, vecType.type, vecType.componentCount);
	}

	template<typename T>
	void MslWriter::Append(const T& param)
	{
		assert(m_currentState && "This function should only be called while processing an AST");

		if (m_currentState->streamEmptyLine > 0)
		{
			for (std::size_t i = 0; i < m_currentState->indentLevel; ++i)
				m_currentState->stream << '\t';

			m_currentState->streamEmptyLine = 0;
		}

		m_currentState->stream << param;
	}

	template<typename T1, typename T2, typename... Args>
	void MslWriter::Append(const T1& firstParam, const T2& secondParam, Args&&... params)
	{
		Append(firstParam);
		Append(secondParam, std::forward<Args>(params)...);
	}

	template<typename... Args>
	void MslWriter::AppendAttributes(bool appendLine, Args&&... params)
	{
		bool hasAnyAttribute = (params.HasValue() || ...);
		if (!hasAnyAttribute)
			return;

		bool first = true;

		AppendAttributesInternal(first, std::forward<Args>(params)...);

		if (appendLine)
			AppendLine();
	}

	template<typename T>
	void MslWriter::AppendAttributesInternal(bool& first, const T& param)
	{
		if (!param.HasValue())
			return;

		if (!first)
			Append(" ");

		first = false;

		AppendAttribute(param);
	}

	template<typename T1, typename T2, typename... Rest>
	void MslWriter::AppendAttributesInternal(bool& first, const T1& firstParam, const T2& secondParam, Rest&&... params)
	{
		AppendAttributesInternal(first, firstParam);
		AppendAttributesInternal(first, secondParam, std::forward<Rest>(params)...);
	}

	void MslWriter::AppendAttribute(AutoBindingAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("auto_binding(");

		if (attribute.autoBinding.IsResultingValue())
			Append((attribute.autoBinding.GetResultingValue()) ? "true" : "false");
		else
			attribute.autoBinding.GetExpression()->Visit(*this);

		Append(")");
	}

	void MslWriter::AppendAttribute(AuthorAttribute attribute)
	{
		if (!attribute.HasValue())
			return;
		AppendComment("Author " + EscapeString(attribute.author));
	}

	void MslWriter::AppendAttribute(BindingAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("binding(");

		if (attribute.bindingIndex.IsResultingValue())
			Append(attribute.bindingIndex.GetResultingValue());
		else
			attribute.bindingIndex.GetExpression()->Visit(*this);

		Append(")");
	}

	void MslWriter::AppendAttribute(BuiltinAttribute attribute)
	{
		if (!attribute.HasValue())
			return;
		auto it = s_mslBuiltinMapping.find(attribute.builtin.GetResultingValue());
		assert(it != s_mslBuiltinMapping.end());
		if (it->second.empty())
			throw std::runtime_error("unsupported builtin attribute!");
		Append("[[", it->second, "]]");
	}

	void MslWriter::AppendAttribute(CondAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("cond(");

		if (attribute.cond.IsResultingValue())
			Append(Ast::ToString(attribute.cond.GetResultingValue()));
		else
			attribute.cond.GetExpression()->Visit(*this);

		Append(")");
	}

	void MslWriter::AppendAttribute(DepthWriteAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("depth_write(");

		if (attribute.writeMode.IsResultingValue())
			Append(Parser::ToString(attribute.writeMode.GetResultingValue()));
		else
			attribute.writeMode.GetExpression()->Visit(*this);

		Append(")");
	}

	void MslWriter::AppendAttribute(DescriptionAttribute attribute)
	{
		if (!attribute.HasValue())
			return;
		AppendComment("Description: " + EscapeString(attribute.description));
	}

	void MslWriter::AppendAttribute(EarlyFragmentTestsAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("early_fragment_tests(");

		if (attribute.earlyFragmentTests.IsResultingValue())
			Append((attribute.earlyFragmentTests.GetResultingValue()) ? "true" : "false");
		else
			attribute.earlyFragmentTests.GetExpression()->Visit(*this);

		Append(")");
	}

	void MslWriter::AppendAttribute(EntryAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		if (attribute.stageType.IsResultingValue())
		{
			switch (attribute.stageType.GetResultingValue())
			{
				case ShaderStageType::Compute: Append("kernel"); break;
				case ShaderStageType::Fragment: Append("fragment"); break;
				case ShaderStageType::Vertex: Append("vertex"); break;
			}
		}
		else
			attribute.stageType.GetExpression()->Visit(*this);
	}

	void MslWriter::AppendAttribute(FeatureAttribute attribute)
	{
		Append("feature(");

		auto it = LangData::s_moduleFeatures.find(attribute.featureAttribute);
		assert(it != LangData::s_moduleFeatures.end());

		Append(it->second.identifier);

		Append(")");
	}

	void MslWriter::AppendAttribute(InterpAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("interp(");

		if (attribute.interpQualifier.IsResultingValue())
			Append(Parser::ToString(attribute.interpQualifier.GetResultingValue()));
		else
			attribute.interpQualifier.GetExpression()->Visit(*this);

		Append(")");
	}

	void MslWriter::AppendAttribute(LangVersionAttribute /*attribute*/)
	{
		// nothing to do
	}

	void MslWriter::AppendAttribute(LayoutAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("layout(");
		if (attribute.layout.IsResultingValue())
			Append(Parser::ToString(attribute.layout.GetResultingValue()));
		else
			attribute.layout.GetExpression()->Visit(*this);
		Append(")");
	}

	void MslWriter::AppendAttribute(LicenseAttribute attribute)
	{
		if (!attribute.HasValue())
			return;
		AppendComment("License: " + EscapeString(attribute.license));
	}

	void MslWriter::AppendAttribute(LocationAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("[[color(");

		if (attribute.locationIndex.IsResultingValue())
			Append(attribute.locationIndex.GetResultingValue());
		else
			attribute.locationIndex.GetExpression()->Visit(*this);

		Append(")]]");
	}

	void MslWriter::AppendAttribute(SetAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("set(");

		if (attribute.setIndex.IsResultingValue())
			Append(attribute.setIndex.GetResultingValue());
		else
			attribute.setIndex.GetExpression()->Visit(*this);

		Append(")");
	}

	void MslWriter::AppendAttribute(TagAttribute attribute)
	{
		if (!attribute.HasValue())
			return;
		AppendComment("Tag: " + attribute.tag);
	}

	void MslWriter::AppendAttribute(UnrollAttribute /*attribute*/)
	{
		throw std::runtime_error("unexpected unroll attribute, is the shader sanitized?");
	}

	void MslWriter::AppendAttribute(WorkgroupAttribute attribute)
	{
		if (!attribute.HasValue())
			return;

		Append("workgroup(");

		if (attribute.workgroup.IsResultingValue())
		{
			const Vector3u32& workgroupSize = attribute.workgroup.GetResultingValue();
			Append(workgroupSize.x(), ", ", workgroupSize.y(), ", ", workgroupSize.z());
		}
		else
		{
			const Ast::ExpressionPtr& workgroupExpr = attribute.workgroup.GetExpression();
			if (workgroupExpr->GetType() != Ast::NodeType::CastExpression)
				throw std::runtime_error("expected workgroup expression to be a cast expression");

			const Ast::CastExpression& workgroupCast = static_cast<const Ast::CastExpression&>(*workgroupExpr);
			if (!workgroupCast.targetType.IsResultingValue() || workgroupCast.targetType.GetResultingValue() != Ast::ExpressionType{ Ast::VectorType{ 3, Ast::PrimitiveType::UInt32 }})
				throw std::runtime_error("expected workgroup expression to be a cast to vec3[u32]");

			if (workgroupCast.expressions.size() != 3)
				throw std::runtime_error("expected workgroup expression to be a cast of 3 expressions");

			workgroupCast.expressions[0]->Visit(*this);
			Append(", ");
			workgroupCast.expressions[1]->Visit(*this);
			Append(", ");
			workgroupCast.expressions[2]->Visit(*this);
		}

		Append(")");
	}

	void MslWriter::AppendComment(std::string_view section)
	{
		std::size_t lineFeed = section.find('\n');
		if (lineFeed != section.npos)
		{
			std::size_t previousCut = 0;

			AppendLine("/*");
			do
			{
				AppendLine(section.substr(previousCut, lineFeed - previousCut));
				previousCut = lineFeed + 1;
			}
			while ((lineFeed = section.find('\n', previousCut)) != section.npos);
			AppendLine(section.substr(previousCut));
			AppendLine("*/");
		}
		else
			AppendLine("// ", section);
	}

	void MslWriter::AppendCommentSection(std::string_view section)
	{
		assert(m_currentState && "This function should only be called while processing an AST");

		std::string stars((section.size() < 33) ? (36 - section.size()) / 2 : 3, '*');
		Append("/*", stars, ' ', section, ' ', stars, "*/");
		AppendLine();
	}

	template<typename T>
	void MslWriter::AppendIdentifier(const T& map, std::size_t id)
	{
		const auto& identifier = Nz::Retrieve(map, id);
		if (identifier.moduleIndex != m_currentState->currentModuleIndex)
			Append(m_currentState->moduleNames[identifier.moduleIndex], '.');
		Append(identifier.name);
	}

	void MslWriter::AppendLine(std::string_view txt)
	{
		assert(m_currentState && "This function should only be called while processing an AST");

		if (txt.empty() && m_currentState->streamEmptyLine > 1)
			return;

		m_currentState->stream << txt << '\n';
		m_currentState->streamEmptyLine++;
	}

	template<typename... Args>
	void MslWriter::AppendLine(Args&&... params)
	{
		(Append(std::forward<Args>(params)), ...);
		AppendLine();
	}

	template<typename T>
	void MslWriter::AppendValue(const T& value)
	{
		if constexpr (std::is_same_v<T, std::vector<bool>::reference>)
		{
			// fallback for std::vector<bool>
			bool v = value;
			return AppendValue(v);
		}
		else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, std::uint32_t>)
		{
			bool hasUntypedLiterals = m_currentState->currentModule->metadata->langVersion >= Version::UntypedLiterals;
			Append(Ast::ToString(value, !hasUntypedLiterals || m_currentState->enforceNonDefaultTypes));
		}
		else
			Append(Ast::ConstantToString(value));
	}

	void MslWriter::AppendHeader(const Ast::Module& module)
	{
		if (!module.metadata->moduleName.empty() && module.metadata->moduleName[0] != '_')
			AppendComment("Module " + EscapeString(module.metadata->moduleName));
		AppendModuleAttributes(*module.metadata);
	}
	void MslWriter::AppendModuleAttributes(const Ast::Module::Metadata& metadata)
	{
		AppendAttributes(true, LangVersionAttribute{ metadata.langVersion });
		for (Ast::ModuleFeature feature : metadata.enabledFeatures)
			AppendAttributes(true, FeatureAttribute{ feature });

		AppendAttributes(true, AuthorAttribute{ metadata.author }, DescriptionAttribute{ metadata.description });
		AppendAttributes(true, LicenseAttribute{ metadata.license });
	}

	void MslWriter::AppendStatementList(std::vector<Ast::StatementPtr>& statements)
	{
		bool first = true;
		for (const Ast::StatementPtr& statement : statements)
		{
			if (statement->GetType() == Ast::NodeType::NoOpStatement)
				continue;

			if (!first)
				AppendLine();

			statement->Visit(*this);

			first = false;
		}
	}

	void MslWriter::EnterScope()
	{
		assert(m_currentState && "This function should only be called while processing an AST");

		AppendLine("{");
		m_currentState->indentLevel++;
	}

	void MslWriter::LeaveScope(bool skipLine)
	{
		assert(m_currentState && "This function should only be called while processing an AST");

		m_currentState->indentLevel--;
		AppendLine();

		if (skipLine)
			AppendLine("}");
		else
			Append("}");
	}

	void MslWriter::RegisterAlias(std::size_t aliasIndex, std::string aliasName)
	{
		State::Identifier identifier;
		identifier.moduleIndex = m_currentState->currentModuleIndex;
		identifier.name = std::move(aliasName);

		assert(m_currentState->aliases.find(aliasIndex) == m_currentState->aliases.end());
		m_currentState->aliases.emplace(aliasIndex, std::move(identifier));
	}

	void MslWriter::RegisterConstant(std::size_t constantIndex, std::string constantName)
	{
		State::Identifier identifier;
		identifier.moduleIndex = m_currentState->currentModuleIndex;
		identifier.name = std::move(constantName);

		assert(m_currentState->constants.find(constantIndex) == m_currentState->constants.end());
		m_currentState->constants.emplace(constantIndex, std::move(identifier));
	}

	void MslWriter::RegisterFunction(std::size_t funcIndex, std::string functionName)
	{
		State::Identifier identifier;
		identifier.moduleIndex = m_currentState->currentModuleIndex;
		identifier.name = std::move(functionName);

		assert(m_currentState->functions.find(funcIndex) == m_currentState->functions.end());
		m_currentState->functions.emplace(funcIndex, std::move(identifier));
	}

	void MslWriter::RegisterModule(std::size_t moduleIndex, std::string moduleName)
	{
		State::Identifier identifier;
		identifier.moduleIndex = m_currentState->currentModuleIndex;
		identifier.name = std::move(moduleName);

		assert(m_currentState->modules.find(moduleIndex) == m_currentState->modules.end());
		m_currentState->modules.emplace(moduleIndex, std::move(identifier));
	}

	void MslWriter::RegisterStruct(std::size_t structIndex, const Ast::StructDescription& structDescription)
	{
		State::StructData structData;
		structData.moduleIndex = m_currentState->currentModuleIndex;
		structData.name = structDescription.name;
		structData.desc = &structDescription;

		assert(m_currentState->structs.find(structIndex) == m_currentState->structs.end());
		m_currentState->structs.emplace(structIndex, std::move(structData));
	}

	void MslWriter::RegisterVariable(std::size_t varIndex, std::string varName)
	{
		State::Identifier identifier;
		identifier.externalBlockIndex = m_currentState->currentExternalBlockIndex;
		identifier.moduleIndex = m_currentState->currentModuleIndex;
		identifier.name = std::move(varName);

		assert(m_currentState->variables.find(varIndex) == m_currentState->variables.end());
		m_currentState->variables.emplace(varIndex, std::move(identifier));
	}

	void MslWriter::ScopeVisit(Ast::Statement& node)
	{
		if (node.GetType() != Ast::NodeType::ScopedStatement)
		{
			EnterScope();
			node.Visit(*this);
			LeaveScope(true);
		}
		else
			node.Visit(*this);
	}

	void MslWriter::Visit(Ast::ExpressionPtr& expr, bool encloseIfRequired)
	{
		bool enclose = encloseIfRequired && (GetExpressionCategory(*expr) == Ast::ExpressionCategory::Temporary);

		if (enclose)
			Append("(");

		expr->Visit(*this);

		if (enclose)
			Append(")");
	}

	void MslWriter::Visit(Ast::AccessFieldExpression& node)
	{
		Visit(node.expr, true);

		const Ast::ExpressionType* exprType = GetExpressionType(*node.expr);
		NazaraUnused(exprType);
		assert(exprType);
		assert(IsStructAddressible(*exprType));

		std::size_t structIndex = Ast::ResolveStructIndex(*exprType);
		assert(structIndex != std::numeric_limits<std::size_t>::max());

		const auto& structData = Nz::Retrieve(m_currentState->structs, structIndex);

		std::uint32_t remainingIndices = node.fieldIndex;
		for (const auto& member : structData.desc->members)
		{
			if (member.cond.HasValue() && !member.cond.GetResultingValue())
				continue;

			if (remainingIndices == 0)
			{
				Append(".", member.name);
				break;
			}

			remainingIndices--;
		}
	}

	void MslWriter::Visit(Ast::AccessIdentifierExpression& node)
	{
		Visit(node.expr, true);

		for (const auto& identifierEntry : node.identifiers)
			Append(".", identifierEntry.identifier);
	}

	void MslWriter::Visit(Ast::AccessIndexExpression& node)
	{
		Visit(node.expr, true);

		// Array access
		Append("[");

		bool first = true;
		for (Ast::ExpressionPtr& expr : node.indices)
		{
			if (!first)
				Append(", ");

			expr->Visit(*this);
			first = false;
		}

		Append("]");
	}

	void MslWriter::Visit(Ast::AssignExpression& node)
	{
		node.left->Visit(*this);

		switch (node.op)
		{
			case Ast::AssignType::Simple:             Append(" = "); break;
			case Ast::AssignType::CompoundAdd:        Append(" += "); break;
			case Ast::AssignType::CompoundDivide:     Append(" /= "); break;
			case Ast::AssignType::CompoundModulo:     Append(" %= "); break;
			case Ast::AssignType::CompoundMultiply:   Append(" *= "); break;
			case Ast::AssignType::CompoundLogicalAnd: Append(" &&= "); break;
			case Ast::AssignType::CompoundLogicalOr:  Append(" ||= "); break;
			case Ast::AssignType::CompoundSubtract:   Append(" -= "); break;
		}

		node.right->Visit(*this);
	}

	void MslWriter::Visit(Ast::BinaryExpression& node)
	{
		Visit(node.left, true);

		switch (node.op)
		{
			case Ast::BinaryType::Add:        Append(" + "); break;
			case Ast::BinaryType::Subtract:   Append(" - "); break;
			case Ast::BinaryType::Modulo:     Append(" % "); break;
			case Ast::BinaryType::Multiply:   Append(" * "); break;
			case Ast::BinaryType::Divide:     Append(" / "); break;

			case Ast::BinaryType::CompEq:     Append(" == "); break;
			case Ast::BinaryType::CompGe:     Append(" >= "); break;
			case Ast::BinaryType::CompGt:     Append(" > ");  break;
			case Ast::BinaryType::CompLe:     Append(" <= "); break;
			case Ast::BinaryType::CompLt:     Append(" < ");  break;
			case Ast::BinaryType::CompNe:     Append(" != "); break;

			case Ast::BinaryType::LogicalAnd: Append(" && "); break;
			case Ast::BinaryType::LogicalOr:  Append(" || "); break;

			case Ast::BinaryType::BitwiseAnd:  Append(" & ");  break;
			case Ast::BinaryType::BitwiseOr:   Append(" | ");  break;
			case Ast::BinaryType::BitwiseXor:  Append(" ^ ");  break;
			case Ast::BinaryType::ShiftLeft:   Append(" << "); break;
			case Ast::BinaryType::ShiftRight:  Append(" >> "); break;
		}

		Visit(node.right, true);
	}

	void MslWriter::Visit(Ast::CallFunctionExpression& node)
	{
		node.targetFunction->Visit(*this);

		Append("(");
		for (std::size_t i = 0; i < node.parameters.size(); ++i)
		{
			if (i != 0)
				Append(", ");
			node.parameters[i].expr->Visit(*this);
		}
		Append(")");
	}

	void MslWriter::Visit(Ast::CastExpression& node)
	{
		// TODO: manage different casts (static_cast / reinterpret_cast)
		Append(node.targetType, '(');

		bool first = true;
		for (const auto& exprPtr : node.expressions)
		{
			if (!first)
				Append(", ");

			first = false;

			exprPtr->Visit(*this);
		}

		Append(")");
	}

	void MslWriter::Visit(Ast::ConditionalExpression& /*node*/)
	{
		throw std::runtime_error("unexpected conditional expression, is shader sanitized?");
	}

	void MslWriter::Visit(Ast::ConstantArrayValueExpression& node)
	{
		Append(*node.cachedExpressionType);
		m_currentState->indentLevel++;
		AppendLine("(");
		std::visit([&](auto&& vec)
		{
			using T = std::decay_t<decltype(vec)>;

			if constexpr (std::is_same_v<T, Ast::NoValue>)
				throw std::runtime_error("unexpected array of NoValue");
			else
			{
				for (std::size_t i = 0; i < vec.size(); ++i)
				{
					if (i != 0)
						AppendLine(",");

					AppendValue(vec[i]);
				}
			}
		}, node.values);
		m_currentState->indentLevel--;
		AppendLine();
		Append(")");
	}

	void MslWriter::Visit(Ast::ConstantValueExpression& node)
	{
		std::visit([&](auto&& arg)
		{
			AppendValue(arg);
		}, node.value);
	}

	void MslWriter::Visit(Ast::IdentifierExpression& node)
	{
		Append(node.identifier);
	}

	void MslWriter::Visit(Ast::IdentifierValueExpression& node)
	{
		switch (node.identifierType)
		{
			case Ast::IdentifierType::Intrinsic:        throw std::runtime_error("unexpected Intrinsic identifier");
			case Ast::IdentifierType::Type:             throw std::runtime_error("unexpected Type identifier");
			case Ast::IdentifierType::Unresolved:       throw std::runtime_error("unexpected Unresolved identifier");

			case Ast::IdentifierType::Alias:
				AppendIdentifier(m_currentState->aliases, node.identifierIndex);
				break;

			case Ast::IdentifierType::Constant:
				AppendIdentifier(m_currentState->constants, node.identifierIndex);
				break;

			case Ast::IdentifierType::ExternalBlock:
				break;

			case Ast::IdentifierType::Function:
				AppendIdentifier(m_currentState->functions, node.identifierIndex);
				break;

			case Ast::IdentifierType::Module:
				AppendIdentifier(m_currentState->modules, node.identifierIndex);
				break;

			case Ast::IdentifierType::Struct:
				AppendIdentifier(m_currentState->structs, node.identifierIndex);
				break;

			case Ast::IdentifierType::Variable:
				AppendIdentifier(m_currentState->variables, node.identifierIndex);
				break;
		}
	}

	void MslWriter::Visit(Ast::IntrinsicExpression& node)
	{
		bool method = false;
		bool firstParam = true;
		switch (node.intrinsic)
		{
			// Function intrinsics
			case Ast::IntrinsicType::Abs:
			case Ast::IntrinsicType::All:
			case Ast::IntrinsicType::Any:
			case Ast::IntrinsicType::ArcCos:
			case Ast::IntrinsicType::ArcCosh:
			case Ast::IntrinsicType::ArcSin:
			case Ast::IntrinsicType::ArcSinh:
			case Ast::IntrinsicType::ArcTan:
			case Ast::IntrinsicType::ArcTan2:
			case Ast::IntrinsicType::ArcTanh:
			case Ast::IntrinsicType::Ceil:
			case Ast::IntrinsicType::Clamp:
			case Ast::IntrinsicType::Cos:
			case Ast::IntrinsicType::Cosh:
			case Ast::IntrinsicType::CrossProduct:
			case Ast::IntrinsicType::DegToRad:
			case Ast::IntrinsicType::Distance:
			case Ast::IntrinsicType::DotProduct:
			case Ast::IntrinsicType::Exp:
			case Ast::IntrinsicType::Exp2:
			case Ast::IntrinsicType::Floor:
			case Ast::IntrinsicType::Fract:
			case Ast::IntrinsicType::InverseSqrt:
			case Ast::IntrinsicType::IsInf:
			case Ast::IntrinsicType::IsNaN:
			case Ast::IntrinsicType::Length:
			case Ast::IntrinsicType::Lerp:
			case Ast::IntrinsicType::Log:
			case Ast::IntrinsicType::Log2:
			case Ast::IntrinsicType::MatrixInverse:
			case Ast::IntrinsicType::MatrixTranspose:
			case Ast::IntrinsicType::Max:
			case Ast::IntrinsicType::Min:
			case Ast::IntrinsicType::Normalize:
			case Ast::IntrinsicType::Not:
			case Ast::IntrinsicType::Pow:
			case Ast::IntrinsicType::RadToDeg:
			case Ast::IntrinsicType::Reflect:
			case Ast::IntrinsicType::Round:
			case Ast::IntrinsicType::RoundEven:
			case Ast::IntrinsicType::Select:
			case Ast::IntrinsicType::Sign:
			case Ast::IntrinsicType::Sin:
			case Ast::IntrinsicType::Sinh:
			case Ast::IntrinsicType::SmoothStep:
			case Ast::IntrinsicType::Sqrt:
			case Ast::IntrinsicType::Step:
			case Ast::IntrinsicType::Tan:
			case Ast::IntrinsicType::Tanh:
			case Ast::IntrinsicType::Trunc:
			{
				auto intrinsicIt = LangData::s_intrinsicData.find(node.intrinsic);
				assert(intrinsicIt != LangData::s_intrinsicData.end());
				assert(!intrinsicIt->second.functionName.empty());

				Append(intrinsicIt->second.functionName);
				break;
			}

			// Method intrinsics
			case Ast::IntrinsicType::ArraySize:
				assert(!node.parameters.empty());
				Visit(node.parameters.front(), true);
				Append(".Size");
				method = true;
				break;

			case Ast::IntrinsicType::TextureRead:
				assert(!node.parameters.empty());
				Visit(node.parameters.front(), true);
				Append(".Read");
				method = true;
				break;

			case Ast::IntrinsicType::TextureSampleImplicitLod:
				assert(!node.parameters.empty());
				Visit(node.parameters.front(), true);
				Append(".sample(");
				Visit(node.parameters.front(), true);
				Append("Sampler, ");
				method = true;
				firstParam = false;
				break;

			case Ast::IntrinsicType::TextureSampleImplicitLodDepthComp:
				assert(!node.parameters.empty());
				Visit(node.parameters.front(), true);
				Append(".SampleDepthComp");
				method = true;
				break;

			case Ast::IntrinsicType::TextureWrite:
				assert(!node.parameters.empty());
				Visit(node.parameters.front(), true);
				Append(".Write");
				method = true;
				break;
		}

		// We have to enforce constant types for intrinsics for the right overload to be used
		bool prevShouldEnforceTypes = m_currentState->enforceNonDefaultTypes;
		m_currentState->enforceNonDefaultTypes = true;

		if (firstParam)
			Append("(");
		bool first = true;
		for (std::size_t i = (method) ? 1 : 0; i < node.parameters.size(); ++i)
		{
			if (!first)
				Append(", ");

			first = false;

			node.parameters[i]->Visit(*this);
		}
		Append(")");

		m_currentState->enforceNonDefaultTypes = prevShouldEnforceTypes;
	}

	void MslWriter::Visit(Ast::SwizzleExpression& node)
	{
		// Force enclose literals (like 1.0.xxx)
		bool forceEnclose = node.expression->GetType() == Ast::NodeType::ConstantValueExpression && IsPrimitiveType(EnsureExpressionType(*node.expression));
		if (forceEnclose)
			Append("(");

		Visit(node.expression, !forceEnclose);

		if (forceEnclose)
			Append(")");

		Append(".");

		const char* componentStr = "xyzw";
		for (std::size_t i = 0; i < node.componentCount; ++i)
			Append(componentStr[node.components[i]]);
	}

	void MslWriter::Visit(Ast::TypeConstantExpression& node)
	{
		Append(node.type, ".", Parser::ToString(node.typeConstant));
	}

	void MslWriter::Visit(Ast::UnaryExpression& node)
	{
		switch (node.op)
		{
			case Ast::UnaryType::BitwiseNot:
				Append("~");
				break;

			case Ast::UnaryType::LogicalNot:
				Append("!");
				break;
			case Ast::UnaryType::Minus:
				Append("-");
				break;

			case Ast::UnaryType::Plus:
				Append("+");
				break;
		}

		node.expression->Visit(*this);
	}

	void MslWriter::Visit(Ast::BranchStatement& node)
	{
		bool first = true;
		for (const auto& statement : node.condStatements)
		{
			if (first)
			{
				if (node.isConst)
					Append("const ");
			}
			else
				Append("else ");

			Append("if (");
			statement.condition->Visit(*this);
			AppendLine(")");

			ScopeVisit(*statement.statement);

			first = false;
		}

		if (node.elseStatement)
		{
			AppendLine("else");

			ScopeVisit(*node.elseStatement);
		}
	}

	void MslWriter::Visit(Ast::BreakStatement& /*node*/)
	{
		Append("break;");
	}

	void MslWriter::Visit(Ast::ConditionalStatement& node)
	{
		Append("[cond(");
		node.condition->Visit(*this);
		AppendLine(")]");
		node.statement->Visit(*this);
	}

	void MslWriter::Visit(Ast::ContinueStatement& /*node*/)
	{
		Append("continue;");
	}

	void MslWriter::Visit(Ast::DeclareAliasStatement& node)
	{
		if (node.aliasIndex)
			RegisterAlias(*node.aliasIndex, node.name);

		Append("alias ", node.name, " = ");
		assert(node.expression);
		node.expression->Visit(*this);

		// Special case, if that alias points to a module, use it instead to try to keep source code readable
		if (node.expression->GetType() == Ast::NodeType::IdentifierValueExpression && static_cast<Ast::IdentifierValueExpression&>(*node.expression).identifierType == Ast::IdentifierType::Module)
		{
			auto& moduleExpr = Nz::SafeCast<Ast::IdentifierValueExpression&>(*node.expression);
			m_currentState->moduleNames[moduleExpr.identifierIndex] = node.name;
		}

		AppendLine(";");
	}

	void MslWriter::Visit(Ast::DeclareConstStatement& node)
	{
		if (node.constIndex)
			RegisterConstant(*node.constIndex, node.name);

		Append("const ", node.type, ' ', node.name);

		if (node.expression)
		{
			Append(" = ");
			node.expression->Visit(*this);
		}

		AppendLine(";");
	}

	void MslWriter::Visit(Ast::DeclareExternalStatement& node)
	{
		AppendLine("struct ", s_mslExternalStructName);
		EnterScope();
		{
			bool first = true;
			for (const auto& externalVar : node.externalVars)
			{
				const Ast::ExpressionType& exprType = externalVar.type.GetResultingValue();

				if (!first)
					AppendLine();
				first = false;

				if (IsUniformType(exprType))
					Append("constant ", externalVar.type, "& ");
				else if (IsStorageType(exprType))
					Append("device ", externalVar.type, "* ");
				else
					Append(externalVar.type, ' ');
				Append(externalVar.name, ' ');
				if (IsUniformType(exprType) || IsStorageType(exprType))
				{
					Append("[[buffer(", m_currentState->externalBuffersCount, ")]]");
					m_currentState->externalBuffersCount++;
				}
				else if (IsSamplerType(exprType) || IsTextureType(exprType))
				{
					Append("[[texture(", m_currentState->externalTexturesCount, ")]]");
					m_currentState->externalTexturesCount++;
				}
				Append(';');
				if (IsSamplerType(exprType))
				{
					AppendLine();
					Append(s_mslNamespace, "sampler ", externalVar.name, "Sampler [[sampler(", (externalVar.bindingSet.GetResultingValue() + 1) * externalVar.bindingIndex.GetResultingValue(), ")]];");
				}

				if (externalVar.varIndex)
					RegisterVariable(*externalVar.varIndex, externalVar.name);
			}
		}
		LeaveScope(false);
		AppendLine(';');
		m_currentState->hasExternalStructDeclared = true;
	}

	void MslWriter::Visit(Ast::DeclareFunctionStatement& node)
	{
		assert(m_currentState && "This function should only be called while processing an AST");

		AppendAttributes(true,
			EntryAttribute{ node.entryStage },
			WorkgroupAttribute{ node.workgroupSize },
			EarlyFragmentTestsAttribute{ node.earlyFragmentTests },
			DepthWriteAttribute{ node.depthWrite }
		);

		if (node.returnType.HasValue() && (!node.returnType.IsResultingValue() || !IsNoType(node.returnType.GetResultingValue())))
			Append(node.returnType);
		else
			Append("void");
		Append(" ", node.name, "(");
		std::size_t i = 0;
		for (; i < node.parameters.size(); ++i)
		{
			const auto& parameter = node.parameters[i];

			if (i != 0)
				Append(',');

			Append(parameter.type, ' ', parameter.name);
			if (i == 0 && node.entryStage.HasValue())
			{
				if (node.entryStage.GetResultingValue() == ShaderStageType::Fragment || node.entryStage.GetResultingValue() == ShaderStageType::Vertex)
					Append(" [[stage_in]]");
			}

			if (parameter.varIndex)
				RegisterVariable(*parameter.varIndex, parameter.name);
		}
		if (m_currentState->hasExternalStructDeclared)
		{
			if (i != 0)
				Append(", ");
			AppendLine(s_mslExternalStructName, " externals)");
		}
		EnterScope();
		{
			AppendStatementList(node.statements);
		}
		LeaveScope();
	}

	void MslWriter::Visit(Ast::DeclareOptionStatement& /*node*/)
	{
		throw std::runtime_error("unexpected option declaration, is shader sanitized?");
	}

	void MslWriter::Visit(Ast::DeclareStructStatement& node)
	{
		if (node.structIndex)
			RegisterStruct(*node.structIndex, node.description);

		AppendAttributes(true, LayoutAttribute{ node.description.layout }, TagAttribute{ node.description.tag });
		Append("struct ");
		AppendLine(node.description.name);
		EnterScope();
		{
			bool first = true;
			for (const auto& member : node.description.members)
			{
				if (!first)
					AppendLine();
				first = false;
				AppendAttributes(false, TagAttribute{ member.tag });
				Append(member.type, ' ', member.name);
				if (member.locationIndex.HasValue() || member.builtin.HasValue())
				{
					Append(' ');
					AppendAttributes(false, LocationAttribute{ member.locationIndex }, BuiltinAttribute{ member.builtin });
				}
				Append(';');
			}
		}
		LeaveScope(false);
		AppendLine(';');
	}

	void MslWriter::Visit(Ast::DeclareVariableStatement& node)
	{
		if (node.varIndex)
			RegisterVariable(*node.varIndex, node.varName);

		if (node.varType.HasValue() && (!node.varType.IsResultingValue() || !IsLiteralType(node.varType.GetResultingValue())))
			Append(node.varType);
		else
			Append("void"); // FIXME

		Append(' ', node.varName);

		if (node.initialExpression)
		{
			Append(" = ");
			node.initialExpression->Visit(*this);
		}

		Append(";");
	}

	void MslWriter::Visit(Ast::DiscardStatement& /*node*/)
	{
		Append("discard;");
	}

	void MslWriter::Visit(Ast::ExpressionStatement& node)
	{
		node.expression->Visit(*this);
		Append(";");
	}

	void MslWriter::Visit(Ast::ForStatement& /*node*/)
	{
		throw std::runtime_error("unexpected for statement, is the shader sanitized?");
	}

	void MslWriter::Visit(Ast::ForEachStatement& /*node*/)
	{
		throw std::runtime_error("unexpected for each statement, is the shader sanitized?");
	}

	void MslWriter::Visit(Ast::ImportStatement& /*node*/)
	{
		throw std::runtime_error("unexpected import statement, is the shader sanitized?");
	}

	void MslWriter::Visit(Ast::MultiStatement& node)
	{
		AppendStatementList(node.statements);
	}

	void MslWriter::Visit(Ast::NoOpStatement& /*node*/)
	{
		/* nothing to do */
	}

	void MslWriter::Visit(Ast::ReturnStatement& node)
	{
		if (node.returnExpr)
		{
			Append("return ");
			node.returnExpr->Visit(*this);
			Append(";");
		}
		else
			Append("return;");
	}

	void MslWriter::Visit(Ast::ScopedStatement& node)
	{
		EnterScope();
		node.statement->Visit(*this);
		LeaveScope();
	}

	void MslWriter::Visit(Ast::WhileStatement& node)
	{
		Append("while (");
		node.condition->Visit(*this);
		AppendLine(")");

		ScopeVisit(*node.body);
	}

}
