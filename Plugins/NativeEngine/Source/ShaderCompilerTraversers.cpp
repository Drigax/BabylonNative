#include "ShaderCompilerTraversers.h"

#include <glslang/Include/intermediate.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <glslang/MachineIndependent/RemoveTree.h>

#include <bgfx/bgfx.h>

#include <arcana/experimental/array.h>

#include <gsl/gsl>

#include <stdexcept>
#include <iostream>
#include <arcana/macros.h>

using namespace glslang;

namespace Babylon::ShaderCompilerTraversers
{
    /// The purpose of everything in this namespace is to modify the glslang abstract
    /// syntax tree generated by parsing Babylon.js shaders so that those shaders
    /// can be recompiled to target native shader languages such as DirectX, OpenGL,
    /// and Metal.
    namespace
    {
        /// Helper method to replace symbols in a glslang AST. This operation is done
        /// by several of the traversers in this file.
        /// @param nameToReplacement Map from symbol names to the node which should replace that symbol.
        /// @param symbolToParent Vector of symbols to be replaced along with their parents in the AST.
        void makeReplacements(
            std::map<std::string, TIntermTyped*> nameToReplacement,
            std::vector<std::pair<TIntermSymbol*, TIntermNode*>> symbolToParent)
        {
            for (const auto& [symbol, parent] : symbolToParent)
            {
                auto* replacement = nameToReplacement[symbol->getName().c_str()];
                if (auto* aggregate = parent->getAsAggregate())
                {
                    auto& sequence = aggregate->getSequence();
                    for (size_t idx = 0; idx < sequence.size(); ++idx)
                    {
                        if (sequence[idx] == symbol)
                        {
                            RemoveAllTreeNodes(sequence[idx]);
                            sequence[idx] = replacement;
                        }
                    }
                }
                else if (auto* binary = parent->getAsBinaryNode())
                {
                    if (binary->getLeft() == symbol)
                    {
                        RemoveAllTreeNodes(binary->getLeft());
                        binary->setLeft(replacement);
                    }
                    else
                    {
                        RemoveAllTreeNodes(binary->getRight());
                        binary->setRight(replacement);
                    }
                }
                else if (auto* unary = parent->getAsUnaryNode())
                {
                    RemoveAllTreeNodes(unary->getOperand());
                    unary->setOperand(replacement);
                }
                else
                {
                    throw std::runtime_error{"Cannot replace symbol: node type handler unimplemented"};
                }
            }
        }

        /// Helper method to determine whether an element in the AST is a linker object,
        /// which is a special part of the AST used to enumerate symbols for linking.
        /// @param path The path to the element in question.
        /// @returns True if path is for a linker object, false otherwise.
        bool isLinkerObject(const TIntermSequence& path)
        {
            auto* agg = path.size() > 1 ? path[1]->getAsAggregate() : nullptr;
            return agg && agg->getOp() == EOpLinkerObjects;
        }

        /// This traverser collects all non-sampler uniforms and creates a new struct
        /// called "Frame" to contain them. This is necessary to correctly transpile
        /// for DirectX and Metal.
        class NonSamplerUniformToStructTraverser final : private TIntermTraverser
        {
        public:
            static ScopeT Traverse(TProgram& program, IdGenerator& ids)
            {
                auto* scope = new AllocationsScope();
                Traverse(program.getIntermediate(EShLangVertex), ids, *scope);
                Traverse(program.getIntermediate(EShLangFragment), ids, *scope);
                return std::unique_ptr<AllocationsScopeBase>(scope);
            }

        private:
            /// This scope contains the new allocations added to the AST to represent
            /// the new struct, information about the elements it contains, dereferencing
            /// operations, etc.
            class AllocationsScope : AllocationsScopeBase
            {
            private:
                friend NonSamplerUniformToStructTraverser;
                std::vector<std::unique_ptr<TType>> Types{};
                std::vector<std::unique_ptr<TTypeList>> TypeLists{};
                std::vector<std::unique_ptr<TArraySizes>> ArraySizes{};
            };

            virtual void visitSymbol(TIntermSymbol* symbol) override
            {
                // Collect all non-sampler uniforms and add the to the list of elements to process.
                if (symbol->getType().getQualifier().isUniformOrBuffer() && symbol->getType().getBasicType() != EbtSampler)
                {
                    // Linker objects are treated differently by this traverser because unlike ordinary
                    // symbols which should simply be replaced with their struct members, the linker
                    // section of the AST must be fundamentally changed to represent the fact that the
                    // new struct exists and that many things that were previously independent linker
                    // objects are now just a part of the new struct.
                    if (isLinkerObject(this->path))
                    {
                        m_uniformNameToSymbol[symbol->getName().c_str()] = symbol;
                    }
                    else
                    {
                        m_symbolsToParents.emplace_back(symbol, this->getParentNode());
                    }
                }
            }

            static void Traverse(TIntermediate* intermediate, IdGenerator& ids, AllocationsScope& scope)
            {
                NonSamplerUniformToStructTraverser traverser{};
                intermediate->getTreeRoot()->traverse(&traverser);

                std::map<std::string, TIntermTyped*> originalNameToReplacement{};

                // Precursor types needed to create subtree replacements.
                TSourceLoc loc{};
                loc.init();
                TPublicType publicType{};
                publicType.qualifier.clearLayout();
                publicType.qualifier.storage = EvqUniform;
                publicType.qualifier.precision = EpqHigh;
                publicType.qualifier.layoutMatrix = ElmColumnMajor;
                publicType.qualifier.layoutPacking = ElpStd140;

                std::vector<std::string> originalNames{};
                scope.TypeLists.emplace_back(std::make_unique<TTypeList>());
                auto* structMembers = scope.TypeLists.back().get();

                // Create all the types for the members of the new struct.
                for (const auto& [name, symbol] : traverser.m_uniformNameToSymbol)
                {
                    const auto& type = symbol->getType();
                    if (type.isMatrix())
                    {
                        publicType.setMatrix(type.getMatrixCols(), type.getMatrixRows());
                    }
                    else if (type.isVector())
                    {
                        publicType.setVector(type.getVectorSize());
                    }
                    else
                    {
                        publicType.setVector(1);
                    }

                    if (type.getArraySizes())
                    {
                        scope.ArraySizes.emplace_back(std::make_unique<TArraySizes>());
                        publicType.arraySizes = scope.ArraySizes.back().get();
                        *publicType.arraySizes = *type.getArraySizes();
                    }
                    else
                    {
                        publicType.arraySizes = nullptr;
                    }

                    scope.Types.emplace_back(std::make_unique<TType>(publicType));
                    auto* newType = scope.Types.back().get();
                    newType->setFieldName(name.c_str());
                    newType->setBasicType(symbol->getType().getBasicType());
                    structMembers->emplace_back();
                    structMembers->back().type = newType;
                    structMembers->back().loc.init();
                }

                // Create the qualifier containing settings for the actual struct itself.
                TQualifier qualifier{};
                qualifier.clearLayout();
                qualifier.storage = EvqUniform;
                qualifier.layoutMatrix = ElmColumnMajor;
                qualifier.layoutPacking = ElpStd140;
                qualifier.layoutBinding = 0; // Determines which cbuffer it's bounds to (b0, b1, b2, etc.)

                // Create the struct type. Name chosen arbitrarily (legacy reasons).
                TType structType(structMembers, "Frame", qualifier);

                // Create the symbol for the actual struct. The name of this symbol, "anon@0",
                // mirrors the kinds of strings glslang generates automatically for these sorts
                // of objects.
                TIntermSymbol* structSymbol = intermediate->addSymbol(TIntermSymbol{ids.Next(), "anon@0", structType});

                // Every affected symbol in the AST (except linker objects) must be replaced
                // with a new operation to retrieve its value from the struct. This operation
                // consists of a binary operation indexing into the struct at a specified
                // index. This loop creates these indexing operations for each of the symbols
                // that must be replaced.
                for (unsigned int idx = 0; idx < structMembers->size(); ++idx)
                {
                    auto& memberType = (*structMembers)[idx].type;

                    auto* left = structSymbol;
                    auto* right = intermediate->addConstantUnion(idx, loc);
                    auto* binary = intermediate->addBinaryNode(EOpIndexDirectStruct, left, right, loc);
                    binary->setType(*memberType);
                    originalNameToReplacement[memberType->getFieldName().c_str()] = binary;
                }

                // Unlike ordinary symbols, linker object symbols must be treated differently
                // because the move to the new struct fundamentally changes the nature of the
                // uniforms they represent. Specifically, anything in the linker object region
                // of the AST which has an analogue in the new struct must be erased to avoid
                // conflicting with the presence of the new struct, which is added to the
                // linker objects sequence at right after the following loop finishes.
                auto* linkerObjectAggregate = intermediate->getTreeRoot()->getAsAggregate()->getSequence().back()->getAsAggregate();
                assert(linkerObjectAggregate->getOp() == EOpLinkerObjects);
                auto& sequence = linkerObjectAggregate->getSequence();
                for (int idx = gsl::narrow_cast<int>(sequence.size()) - 1; idx >= 0; --idx)
                {
                    auto* symbol = sequence[idx]->getAsSymbolNode();
                    if (symbol)
                    {
                        auto found = traverser.m_uniformNameToSymbol.find(symbol->getName().c_str());
                        if (found != traverser.m_uniformNameToSymbol.end())
                        {
                            RemoveAllTreeNodes(symbol);
                            sequence.erase(sequence.begin() + idx);
                        }
                    }
                }
                sequence.insert(sequence.begin(), structSymbol);

                // Replace all remaining occurrances of the affected symbols with the new
                // operations retrieving them from the struct.
                makeReplacements(originalNameToReplacement, traverser.m_symbolsToParents);
            }

            std::map<std::string, TIntermSymbol*> m_uniformNameToSymbol{};
            std::vector<std::pair<TIntermSymbol*, TIntermNode*>> m_symbolsToParents{};
        };

        /// Changes the types of all float, vec2, and vec3 uniforms to vec4. This is required
        /// for OpenGL and Metal.
        class UniformTypeChangeTraverser final : private TIntermTraverser
        {
        public:
            static ScopeT Traverse(TProgram& program, IdGenerator& ids)
            {
                auto* scope = new AllocationsScope();
                Traverse(program.getIntermediate(EShLangVertex), ids, *scope);
                Traverse(program.getIntermediate(EShLangFragment), ids, *scope);
                return std::unique_ptr<AllocationsScopeBase>(scope);
            }

        private:
            class AllocationsScope : AllocationsScopeBase
            {
            private:
                friend UniformTypeChangeTraverser;
                std::vector<std::unique_ptr<TArraySizes>> ArraySizes{};
            };

            UniformTypeChangeTraverser(TIntermediate* intermediate, AllocationsScope& scope)
                : m_intermediate{intermediate}
                , m_scope{scope}
            {
            }

            /// Because no accumulation or cross-correlation is necessary for this change
            /// (i.e. each operation acts only on a single symbol), we do all the work inside
            /// the traverser itself.
            virtual void visitSymbol(TIntermSymbol* symbol) override
            {
                auto& type = symbol->getType();

                // We only care about uniforms that are neither samplers nor matrices.
                if (type.getQualifier().isUniformOrBuffer() && type.getBasicType() != EbtSampler && !type.isMatrix())
                {
                    // At present, this may end up creating layered swizzles; i.e., if a vec3 was already being projected
                    // down a la vec3.x, greedily adding a swizzle operator to deal with the new type mismatch may create
                    // something like (vec3.xyz).x. I suspect this is unlikely to cause problems.

                    auto* oldType = type.clone();

                    TPublicType publicType{};
                    publicType.qualifier = type.getQualifier();

                    publicType.basicType = EbtFloat;
                    publicType.setVector(4);

                    if (type.getArraySizes())
                    {
                        m_scope.ArraySizes.emplace_back(std::make_unique<TArraySizes>());
                        publicType.arraySizes = m_scope.ArraySizes.back().get();
                        *publicType.arraySizes = *type.getArraySizes();
                    }
                    else
                    {
                        publicType.arraySizes = nullptr;
                    }

                    TType newType{publicType};
                    symbol->setType(newType);

                    // Helper function to correctly insert the shape conversion into the AST.
                    // More about shape conversion below.
                    constexpr auto injectShapeConversion = [](TIntermTyped* node, TIntermNode* parent, TIntermTyped* shapeConversion) {
                        if (auto* aggregate = parent->getAsAggregate())
                        {
                            auto& sequence = aggregate->getSequence();
                            for (size_t idx = 0; idx < sequence.size(); ++idx)
                            {
                                if (sequence[idx] == node)
                                {
                                    sequence[idx] = shapeConversion;
                                }
                            }
                        }
                        else if (auto* binary = parent->getAsBinaryNode())
                        {
                            if (binary->getLeft() == node)
                            {
                                binary->setLeft(shapeConversion);
                            }
                            else
                            {
                                binary->setRight(shapeConversion);
                            }
                        }
                        else if (auto* unary = parent->getAsUnaryNode())
                        {
                            unary->setOperand(shapeConversion);
                        }
                        else
                        {
                            throw std::runtime_error{"Cannot replace symbol: node type handler unimplemented"};
                        }
                    };

                    // Because we modified the original symbol, we don't need to do anything to linker objects.
                    // The only further work we need to do is to handle reshaping.
                    if (!isLinkerObject(this->path))
                    {
                        // Reshaping (or, perhaps more commonly, swizzling) must be explicitly done on certain
                        // platforms to resolve discrepancies between the size of the data provided by the new
                        // vec4 and the size of the data expected by whatever was consuming the original uniform.
                        // Fortunately, the glslang intermediate representation makes it reasonably simple to
                        // create a shape conversion operation -- unless the uniform is an array, in which case
                        // it's slightly more complicated.
                        auto* parent = this->getParentNode();
                        if (symbol->isArray())
                        {
                            // Converting the shape of an element retrieved from an array is similar to converting
                            // shape in every other circumstance except for where the conversion needs to happen.
                            // With a typical symbol, the symbol is being "consumed" by its parent node, and so
                            // it is sufficient to introduce a shape conversion between the symbol and its parent
                            // in order to reconcile the vector sizes. In the case of an array, however, the symbol
                            // is not directly being "consumed" by its parent because the parent is actually an
                            // indexing operation that retrieves the data for consumption by THAT node's parent.
                            // This case, then, requires two modifications to the typical behavior: (1) the indexing
                            // operation which is retrieving the value from the array must have its type modified
                            // to correctly represent the type it's retrieving, and (2) a shape conversion must be
                            // inserted between the retrieval operation and its parent, not between the array and
                            // the retrieval operation.
                            if (auto* binary = parent->getAsBinaryNode())
                            {
                                delete oldType;
                                oldType = binary->getType().clone();
                                auto* binType = newType.clone();
                                binType->clearArraySizes();
                                binary->setType(*binType);
                                auto shapeConversion = m_intermediate->addShapeConversion(*oldType, binary);

                                assert(this->path.size() > 1);
                                auto* grandparent = this->path[this->path.size() - 2];
                                injectShapeConversion(binary, grandparent, shapeConversion);
                            }
                            else
                            {
                                throw std::runtime_error{"Cannot replace symbol: array indexing handler unimplemented"};
                            }
                        }
                        else
                        {
                            auto shapeConversion = m_intermediate->addShapeConversion(*oldType, symbol);
                            injectShapeConversion(symbol, parent, shapeConversion);
                        }
                    }

                    delete oldType;
                }
            }

            static void Traverse(TIntermediate* intermediate, IdGenerator&, AllocationsScope& scope)
            {
                UniformTypeChangeTraverser traverser{intermediate, scope};
                intermediate->getTreeRoot()->traverse(&traverser);
            }

            TIntermediate* m_intermediate{};
            AllocationsScope& m_scope;
        };

        /// This traverser modifies all vertex attributes (position, UV, etc.) to conform to
        /// bgfx's expectations regarding name and location. It is currently required for
        /// DirectX, OpenGL, and Metal.
        class VertexVaryingInTraverser final : private TIntermTraverser
        {
        public:
            static void Traverse(TProgram& program, IdGenerator& ids, std::unordered_map<std::string, std::string>& replacementToOriginalName)
            {
                Traverse(program.getIntermediate(EShLangVertex), ids, replacementToOriginalName);
            }

        private:
            virtual void visitSymbol(TIntermSymbol* symbol) override
            {
                // Collect all vertex attributes, described by glslang as "varyings."
                if (symbol->getType().getQualifier().storage == EvqVaryingIn)
                {
                    // Limit this cache to linker objects because we know they will comprehensively
                    // include varyings and will list each only once, making this map as predictable
                    // as possible.
                    if (isLinkerObject(this->path))
                    {
                        m_varyingNameToSymbol[symbol->getName().c_str()] = symbol;
                    }

                    // Because the symbol replacement for varyings is just a new symbol with the
                    // correct parameters, we can just do the linker object replacement alongside
                    // the other replacements, so we add the occurrence here regardless of whether
                    // we're in a linker object.
                    m_symbolsToParents.emplace_back(symbol, this->getParentNode());
                }
            }

#if __APPLE__ | APIOpenGL
            // This table is a copy of the table bgfx uses for vertex attribute -> shader symbol association.
            // copied from renderer_gl.cpp.
            constexpr static const char * s_attribName[] =
            {
                "a_position",
                "a_normal",
                "a_tangent",
                "a_bitangent",
                "a_color0",
                "a_color1",
                "a_color2",
                "a_color3",
                "a_indices",
                "a_weight",
                "a_texcoord0",
                "a_texcoord1",
                "a_texcoord2",
                "a_texcoord3",
                "a_texcoord4",
                "a_texcoord5",
                "a_texcoord6",
                "a_texcoord7",
            };
#endif

            std::pair<unsigned int, const char*> GetVaryingLocationAndNewNameForName(const char* name)
            {
#define IF_NAME_RETURN_ATTRIB(varyingName, attrib, newName)  \
    if (std::strcmp(name, varyingName) == 0)                 \
    {                                                        \
        return {static_cast<unsigned int>(attrib), newName}; \
    }
#if __APPLE__ | APIOpenGL
                // For OpenGL and Metal platforms, we have an issue where we have a hard limit on the number shader attributes supported.
                // To work around this issue, instead of mapping our attributes to the most similar bgfx::attribute, instead replace
                // the first attribute encountered with the symbol bgfx uses for attribute 0 and increment for each subsequent attribute encountered.
                // This will cause our shader to have nonsensical naming, but will allow us to efficiently "pack" the attributes.
                UNUSED(name);
                return {static_cast<unsigned int>(m_genericAttributesRunningCount), s_attribName[m_genericAttributesRunningCount++]};
#else
                IF_NAME_RETURN_ATTRIB("position", bgfx::Attrib::Position, "a_position")
                IF_NAME_RETURN_ATTRIB("normal", bgfx::Attrib::Normal, "a_normal")
                IF_NAME_RETURN_ATTRIB("tangent", bgfx::Attrib::Tangent, "a_tangent")
                IF_NAME_RETURN_ATTRIB("uv", bgfx::Attrib::TexCoord0, "a_texcoord0")
                IF_NAME_RETURN_ATTRIB("uv2", bgfx::Attrib::TexCoord1, "a_texcoord1")
                IF_NAME_RETURN_ATTRIB("uv3", bgfx::Attrib::TexCoord2, "a_texcoord2")
                IF_NAME_RETURN_ATTRIB("uv4", bgfx::Attrib::TexCoord3, "a_texcoord3")
                IF_NAME_RETURN_ATTRIB("color", bgfx::Attrib::Color0, "a_color0")
                IF_NAME_RETURN_ATTRIB("matricesIndices", bgfx::Attrib::Indices, "a_indices")
                IF_NAME_RETURN_ATTRIB("matricesWeights", bgfx::Attrib::Weight, "a_weight")
#undef IF_NAME_RETURN_ATTRIB
                return {FIRST_GENERIC_ATTRIBUTE_LOCATION + m_genericAttributesRunningCount++, name};
#endif
            }

            static void Traverse(TIntermediate* intermediate, IdGenerator& ids, std::unordered_map<std::string, std::string>& replacementToOriginalName)
            {
                VertexVaryingInTraverser traverser{};
                intermediate->getTreeRoot()->traverse(&traverser);

                std::map<std::string, TIntermTyped*> originalNameToReplacement{};

                // Precursor types needed to create subtree replacements.
                TSourceLoc loc{};
                loc.init();
                TPublicType publicType{};
                publicType.qualifier.clearLayout();

#if !(__APPLE__ | APIOpenGL)
                // UVs are effectively a special kind of generic attribute since they both use
                // are implemented using texture coordinates, so we preprocess to pre-count the
                // number of UV coordinate variables to prevent collisions.
                for (const auto& [name, symbol] : traverser.m_varyingNameToSymbol)
                {
                    if (name.size() >= 2 && name[0] == 'u' && name[1] == 'v')
                    {
                        traverser.m_genericAttributesRunningCount++;
                    }
                }
#endif
                // Create the new symbols with which to replace all of the original varying
                // symbols. The primary purpose of these new symbols is to contain the required
                // name and location.
                for (const auto& [name, symbol] : traverser.m_varyingNameToSymbol)
                {
                    const auto& type = symbol->getType();
                    publicType.qualifier = type.getQualifier();
                    auto [location, newName] = traverser.GetVaryingLocationAndNewNameForName(name.c_str());
                    // It may not be necessary to specify this on certain platforms (like OpenGL),
                    // which might simplify the handling of scenarios where we currently run out
                    // of attribute locations.
                    publicType.qualifier.layoutLocation = location;

                    if (type.isMatrix())
                    {
                        publicType.setMatrix(type.getMatrixCols(), type.getMatrixRows());
                    }
                    else if (type.isVector())
                    {
                        publicType.setVector(type.getVectorSize());
                    }

                    TType newType{publicType};
                    newType.setBasicType(symbol->getType().getBasicType());
                    auto* newSymbol = intermediate->addSymbol(TIntermSymbol{ids.Next(), newName, newType});
                    originalNameToReplacement[name] = newSymbol;
                    replacementToOriginalName[newName] = name;
                }

                makeReplacements(originalNameToReplacement, traverser.m_symbolsToParents);
            }
# if !(__APPLE__ | APIOpenGL)
            const unsigned int FIRST_GENERIC_ATTRIBUTE_LOCATION{10};
# endif
            unsigned int m_genericAttributesRunningCount{0};
            std::map<std::string, TIntermSymbol*> m_varyingNameToSymbol{};
            std::vector<std::pair<TIntermSymbol*, TIntermNode*>> m_symbolsToParents{};
        };

        /// <summary>
        /// Split sampler symbols into separate sampler and texture symbols. This is
        /// required for DirectX, OpenGL, and Metal.
        /// </summary>
        class SamplerSplitterTraverser final : TIntermTraverser
        {
        public:
            void visitSymbol(TIntermSymbol* symbol) override
            {
                if (symbol->getType().getQualifier().storage == EvqUniform && symbol->getType().getBasicType() == EbtSampler)
                {
                    // Collect all sampler uniform symbols into the relevant caches
                    // later proccessing. Note that we treat linker object replacement
                    // differently in this traverser, so we don't add linker object symbols
                    // to the m_symbolsToParents cache.
                    if (isLinkerObject(this->path))
                    {
                        m_samplerNameToSymbol[symbol->getName().c_str()] = symbol;
                    }
                    else
                    {
                        m_symbolsToParents.emplace_back(symbol, this->getParentNode());
                    }
                }
            }

            static void Traverse(TProgram& program, IdGenerator& ids)
            {
                Traverse(program.getIntermediate(EShLangVertex), ids);
                Traverse(program.getIntermediate(EShLangFragment), ids);
            }

        private:
            static void Traverse(TIntermediate* intermediate, IdGenerator& ids)
            {
                SamplerSplitterTraverser traverser{};
                intermediate->getTreeRoot()->traverse(&traverser);

                TSourceLoc loc{};
                loc.init();

                std::map<std::string, TIntermTyped*> nameToReplacement{};
                std::map<std::string, std::pair<TIntermSymbol*, TIntermSymbol*>> nameToNewTextureAndSampler{};

                // Create all the new replacers.
                unsigned int layoutBinding = 0;
                for (const auto& [name, symbol] : traverser.m_samplerNameToSymbol)
                {
                    // For each name and symbol, create a replacer.
                    const auto& type = symbol->getType();

                    // Create the new texture symbol.
                    TIntermSymbol* newTexture;
                    {
                        TPublicType publicType{};
                        publicType.qualifier.clearLayout();
                        publicType.basicType = type.getBasicType();
                        publicType.qualifier = type.getQualifier();
                        publicType.qualifier.precision = EpqHigh;
                        publicType.qualifier.layoutBinding = layoutBinding;
                        publicType.sampler = type.getSampler();
                        publicType.sampler.combined = false;

                        TType newType{publicType};
                        std::string newName = name + "Texture";
                        newTexture = intermediate->addSymbol(TIntermSymbol{ids.Next(), newName.c_str(), newType});
                    }

                    // Create the new sampler symbol.
                    TIntermSymbol* newSampler;
                    {
                        TPublicType publicType{};
                        publicType.qualifier.clearLayout();
                        publicType.basicType = type.getBasicType();
                        publicType.qualifier = type.getQualifier();
                        publicType.qualifier.precision = EpqHigh;
                        publicType.qualifier.layoutBinding = layoutBinding;
                        publicType.sampler.sampler = true;

                        TType newType{publicType};
                        newSampler = intermediate->addSymbol(TIntermSymbol{ids.Next(), name.c_str(), newType});
                    }

                    nameToNewTextureAndSampler[name] = std::pair<TIntermSymbol*, TIntermSymbol*>{newTexture, newSampler};

                    // Create the aggregate. This represents the operation that uses the new
                    // texture and sampler symbols to do what was intended by the original
                    // sampler symbol in the source code from Babylon.js.
                    auto* aggregate = intermediate->growAggregate(newTexture, newSampler);
                    {
                        aggregate->setOperator(EOpConstructTextureSampler);

                        TPublicType publicType{};
                        publicType.basicType = type.getBasicType();
                        publicType.qualifier.clearLayout();
                        publicType.qualifier.storage = EvqTemporary;
                        publicType.sampler = type.getSampler();
                        publicType.sampler.combined = true;
                        aggregate->setType(TType{publicType});
                    }

                    nameToReplacement[name] = aggregate;
                    ++layoutBinding;
                }

                // Perform linker object replacements.
                auto* linkerObjectAggregate = intermediate->getTreeRoot()->getAsAggregate()->getSequence().back()->getAsAggregate();
                assert(linkerObjectAggregate->getOp() == EOpLinkerObjects);
                auto& sequence = linkerObjectAggregate->getSequence();
                for (int idx = gsl::narrow_cast<int>(sequence.size()) - 1; idx >= 0; --idx)
                {
                    auto* symbol = sequence[idx]->getAsSymbolNode();
                    if (symbol)
                    {
                        auto found = nameToNewTextureAndSampler.find(symbol->getName().c_str());
                        if (found != nameToNewTextureAndSampler.end())
                        {
                            // Wherever we find a former sampler that has been replaced by a
                            // new pair of symbols, we need to delete the old symbol, replace
                            // it at its position with the new texture symbol, then insert
                            // the new sampler symbol after that. (The order doesn't really
                            // seem to matter, but it makes diffing the debug outputs of the
                            // intermediate representations easier if things that logically
                            // belong together are listed together.)
                            RemoveAllTreeNodes(symbol);
                            sequence[idx] = found->second.first;
                            sequence.insert(sequence.begin() + idx + 1, found->second.second);
                        }
                    }
                }

                makeReplacements(nameToReplacement, traverser.m_symbolsToParents);
            }

            std::map<std::string, TIntermSymbol*> m_samplerNameToSymbol{};
            std::vector<std::pair<TIntermSymbol*, TIntermNode*>> m_symbolsToParents{};
        };

        class InvertYDerivativeOperandsTraverser : public TIntermTraverser
        {
        public:
            static void Traverse(TProgram& program)
            {
                auto intermediate{program.getIntermediate(EShLangFragment)};
                InvertYDerivativeOperandsTraverser invertYDerivativeOperandsTraverser{intermediate};
                intermediate->getTreeRoot()->traverse(&invertYDerivativeOperandsTraverser);
            }

        protected:
            virtual bool visitUnary(TVisit visit, TIntermUnary* unary) override
            {
                if (visit == EvPreVisit)
                {
                    auto op = unary->getOp();
                    if (op == EOpDPdy || op == EOpDPdyFine || op == EOpDPdyCoarse)
                    {
                        unary->setOperand(m_intermediate->addUnaryNode(EOpNegative, unary->getOperand(), {}));
                        return false;
                    }
                }

                return true;
            }

        private:
            InvertYDerivativeOperandsTraverser(TIntermediate* intermediate)
                : m_intermediate{intermediate}
            {
            }

            TIntermediate* m_intermediate;
        };
    }

    ScopeT MoveNonSamplerUniformsIntoStruct(TProgram& program, IdGenerator& ids)
    {
        return NonSamplerUniformToStructTraverser::Traverse(program, ids);
    }

    ScopeT ChangeUniformTypes(TProgram& program, IdGenerator& ids)
    {
        return UniformTypeChangeTraverser::Traverse(program, ids);
    }

    void AssignLocationsAndNamesToVertexVaryings(TProgram& program, IdGenerator& ids, std::unordered_map<std::string, std::string>& replacementToOriginalName)
    {
        VertexVaryingInTraverser::Traverse(program, ids, replacementToOriginalName);
    }

    void SplitSamplersIntoSamplersAndTextures(TProgram& program, IdGenerator& ids)
    {
        SamplerSplitterTraverser::Traverse(program, ids);
    }

    void InvertYDerivativeOperands(TProgram& program)
    {
        InvertYDerivativeOperandsTraverser::Traverse(program);
    }
}
