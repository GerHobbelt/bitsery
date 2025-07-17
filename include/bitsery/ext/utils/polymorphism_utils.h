//MIT License
//
//Copyright (c) 2018 Mindaugas Vinkelis
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files (the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.

#ifndef BITSERY_EXT_POLYMORPHISM_UTILS_H
#define BITSERY_EXT_POLYMORPHISM_UTILS_H

#include <unordered_map>
#include <memory>
#include "memory_resource.h"
#include "../../details/adapter_common.h"
#include "../../details/serialization_common.h"
#include "../../traits/string.h"

namespace bitsery {

    namespace ext {

        //helper type, that contains list of types
        template<typename ...>
        struct PolymorphicClassesList {
        };

        //specialize for your base class by deriving from PolymorphicDerivedClasses with list of derivatives that DIRECTLY inherits from your base class.
//e.g.
// template <> PolymorphicBaseClass<Animal>: PolymorphicDerivedClasses<Dog, Cat>{};
// template <> PolymorphicBaseClass<Dog>: PolymorphicDerivedClasses<Bulldog, GoldenRetriever> {};
// IMPORTANT !!!
// although you can add all derivates to same base like this:
// template <> PolymorphicBaseClass<Animal>:PolymorphicDerivedClasses<Dog, Cat, Bulldog, GoldenRetriever>{};
// it will not work when you try to serialize Dog*, because it will not find Bulldog and GoldenRetriever
        template<typename TBase>
        struct PolymorphicBaseClass {
            using Childs = PolymorphicClassesList<>;
        };
        
        template<typename T>
        struct PolymorphicClassName {
        };

//derive from this class when specifying childs for your base class, atleast one child must exists, hence T1
//e.g.
// template <> PolymorphicBaseClass<Animal>: PolymorphicDerivedClasses<Dog, Cat>{};
        template<typename T1, typename ... Tn>
        struct PolymorphicDerivedClasses {
            using Childs = PolymorphicClassesList<T1, Tn...>;
        };

        class PolymorphicHandlerBase {
        public:
            virtual void* create(const pointer_utils::PolyAllocWithTypeId& alloc) const = 0;

            virtual void destroy(const pointer_utils::PolyAllocWithTypeId& alloc, void* ptr) const = 0;

            virtual void process(void* ser, void* obj) const = 0;

            virtual ~PolymorphicHandlerBase() = default;
        };

        template<typename RTTI, typename TSerializer, typename TBase, typename TDerived>
        class PolymorphicHandler : public PolymorphicHandlerBase {
        public:

            void* create(const pointer_utils::PolyAllocWithTypeId& alloc) const final {
                return toBase(alloc.newObject<TDerived>(RTTI::template get<TDerived>()));
            }

            void destroy(const pointer_utils::PolyAllocWithTypeId& alloc, void* ptr) const final {
                alloc.deleteObject<TDerived>(fromBase(ptr), RTTI::template get<TDerived>());
            }

            void process(void* ser, void* obj) const final {
                static_cast<TSerializer*>(ser)->object(*fromBase(obj));
            }

        private:

            TDerived* fromBase(void* obj) const {
                return RTTI::template cast<TBase, TDerived>(static_cast<TBase*>(obj));
            }

            TBase* toBase(void* obj) const {
                return RTTI::template cast<TDerived, TBase>(static_cast<TDerived*>(obj));
            }

        };

        template<typename RTTI>
        class PolymorphicContext {
        private:

            using Name2DerivedMap = std::unordered_map<std::string, size_t>;
            using Derived2NameMap = std::unordered_map<size_t, std::string>;
            struct Maps
            {
                Name2DerivedMap name2derived;
                Derived2NameMap derived2name;
            };

            struct BaseToDerivedKey {

                std::size_t baseHash;
                std::size_t derivedHash;

                bool operator==(const BaseToDerivedKey& other) const {
                    return baseHash == other.baseHash && derivedHash == other.derivedHash;
                }
            };

            struct BaseToDerivedKeyHashier {
                size_t operator()(const BaseToDerivedKey& key) const {
                    return (key.baseHash + (key.baseHash << 6) + (key.derivedHash >> 2)) ^ key.derivedHash;
                }
            };

            template<typename TSerializer, template<typename> class THierarchy, typename TBase, typename TDerived>
            void add() {
                addToMap<TSerializer, TBase, TDerived>(std::is_abstract<TDerived>{}, PolymorphicClassName<TDerived>::name);
                addChilds<TSerializer, THierarchy, TBase, TDerived>(typename THierarchy<TDerived>::Childs{});
            }

            template<typename TSerializer, template<typename> class THierarchy, typename TBase, typename TDerived, typename T1, typename ... Tn>
            void addChilds(PolymorphicClassesList<T1, Tn...>) {
                static_assert(std::is_base_of<TDerived, T1>::value,
                              "PolymorphicBaseClass<TBase> must derive a list of derived classes from TBase.");
                add<TSerializer, THierarchy, TBase, T1>();
                addChilds<TSerializer, THierarchy, TBase, TDerived>(PolymorphicClassesList<Tn...>{});
                //iterate through derived class hierarchy as well
                add<TSerializer, THierarchy, T1, T1>();
            }

            template<typename TSerializer, template<typename> class THierarchy, typename TBase, typename TDerived>
            void addChilds(PolymorphicClassesList<>) {
            }

            template<typename TSerializer, typename TBase, typename TDerived>
            void addToMap(std::false_type, const char* name) {
                using THandler = PolymorphicHandler<RTTI, TSerializer, TBase, TDerived>;
                BaseToDerivedKey key{RTTI::template get<TBase>(), RTTI::template get<TDerived>()};
                pointer_utils::StdPolyAlloc<THandler> alloc{_memResource};
                auto ptr = alloc.allocate(1);
                std::shared_ptr<THandler> handler(new (ptr)THandler{}, [alloc](THandler* data) mutable {
                        data->~THandler();
                        alloc.deallocate(data, 1);
                    }, alloc);
                if (_baseToDerivedMap
                    .emplace(key, std::move(handler))
                    .second) {
                    auto it = _baseToDerivedArray.find(key.baseHash);
                    if (it == _baseToDerivedArray.end()) {
                        it = _baseToDerivedArray.emplace(key.baseHash, Maps{}).first;
                    }
                    it->second.name2derived.emplace(name, key.derivedHash);
                    it->second.derived2name.emplace(key.derivedHash, name);
                }
            }

            template<typename TSerializer, typename TBase, typename TDerived>
            void addToMap(std::true_type, const char* ) {
                //cannot add abstract class
            }

            MemResourceBase* _memResource;
            // store shared ptr to polymorphic handler, because it might be copied to "smart pointer" deleter
            std::unordered_map<BaseToDerivedKey, std::shared_ptr<PolymorphicHandlerBase>,
                BaseToDerivedKeyHashier, std::equal_to<BaseToDerivedKey>,
                    pointer_utils::StdPolyAlloc<std::pair<const BaseToDerivedKey, std::shared_ptr<PolymorphicHandlerBase>>>
            > _baseToDerivedMap;
            // this will allow convert from platform specific type information, to platform independent base->derived index
            // this only works if all polymorphic relationships (PolymorphicBaseClass<TBase> -> PolymorphicDerivedClasses<TDerived...>)
            // is equal between platforms.
            std::unordered_map<size_t, Maps,
                std::hash<size_t>, std::equal_to<size_t>,
                pointer_utils::StdPolyAlloc<std::pair<const size_t, Maps>>
                > _baseToDerivedArray;

        public:

            explicit PolymorphicContext(MemResourceBase* memResource = nullptr)
                :_memResource{memResource},
                _baseToDerivedMap{0, BaseToDerivedKeyHashier{}, std::equal_to<BaseToDerivedKey>{}, pointer_utils::StdPolyAlloc<std::pair<const BaseToDerivedKey, std::shared_ptr<PolymorphicHandlerBase>>>{memResource}},
                _baseToDerivedArray{0, std::hash<size_t>{}, std::equal_to<size_t>{}, pointer_utils::StdPolyAlloc<std::pair<const size_t, Maps>>{memResource}} {}

            PolymorphicContext(const PolymorphicContext& ) = delete;
            PolymorphicContext& operator = (const PolymorphicContext&) = delete;
            PolymorphicContext(PolymorphicContext&& ) = default;
            PolymorphicContext& operator = (PolymorphicContext&&) = default;


            void clear() {
                _baseToDerivedMap.clear();
                _baseToDerivedArray.clear();
            }

            // THierarchy is the name of class, that defines hierarchy
            // PolymorphicBaseClass is defined as default parameter, so that at instantiation time
            // it will get unique symbol in translation unit for PolymorphicBaseClass (which is defined in anonymous namespace)
            // https://github.com/fraillt/bitsery/issues/9
            template<typename TSerializer, template<typename> class THierarchy = PolymorphicBaseClass, typename T1, typename ...Tn>
            void registerBasesList(PolymorphicClassesList<T1, Tn...>) {
                add<TSerializer, THierarchy, T1, T1>();
                registerBasesList<TSerializer, THierarchy>(PolymorphicClassesList<Tn...>{});
            }

            template<typename TSerializer, template<typename> class THierarchy>
            void registerBasesList(PolymorphicClassesList<>) {
            }

            // optional method, in case you want to construct base class hierarchy your self
            template<typename TSerializer, typename TBase, typename TDerived>
            void registerSingleBaseBranch(const char* name) {
                static_assert(std::is_base_of<TBase, TDerived>::value, "TDerived must be derived from TBase");
                static_assert(!std::is_abstract<TDerived>::value, "TDerived cannot be abstract");
                addToMap<TSerializer, TBase, TDerived>(std::false_type{}, name);
            }


            template<typename Serializer, typename TBase>
            void serialize(Serializer& ser, TBase& obj) const {
                //get derived key
                BaseToDerivedKey key{RTTI::template get<TBase>(), RTTI::template get<TBase>(obj)};
                auto it = _baseToDerivedMap.find(key);
                if (it == _baseToDerivedMap.end())
                  throw std::runtime_error("PolymorphicBaseClass not registered");

                //convert derived hash to derived index, to make it work in cross-platform environment
                auto& map = _baseToDerivedArray.find(key.baseHash)->second.derived2name;
                auto& name = map.at(key.derivedHash);
                ser.text1b(name, name.max_size());

                //serialize
                it->second->process(&ser, &obj);
            }

            template<typename Deserializer, typename TBase, typename TCreateFnc, typename TDestroyFnc>
            void deserialize(Deserializer& des, TBase* obj,
                             TCreateFnc createFnc, TDestroyFnc destroyFnc) const {
                std::string name;
                des.text1b(name, name.max_size());

                auto baseToDerivedVecIt = _baseToDerivedArray.find(RTTI::template get<TBase>());
                //base class is known at compile time, so we can assert on this one
                assert(baseToDerivedVecIt != _baseToDerivedArray.end());
                //convert derived index to derived hash, to make it work in cross-platform environment
                auto derivedHash = baseToDerivedVecIt->second.name2derived.at(name.c_str());
                auto& handler = _baseToDerivedMap.find(
                    BaseToDerivedKey{RTTI::template get<TBase>(), derivedHash})->second;
                //if object is null or different type, create new and assign it
                if (obj == nullptr || RTTI::template get<TBase>(*obj) != derivedHash) {
                    if (obj) {
                        destroyFnc(getPolymorphicHandler(*obj));
                    }
                    obj = createFnc(handler);
                }
                handler->process(&des, obj);
            }

            template<typename TBase>
            const std::shared_ptr<PolymorphicHandlerBase>& getPolymorphicHandler(TBase& obj) const {
                auto deleteHandlerIt = _baseToDerivedMap.find(
                    BaseToDerivedKey{RTTI::template get<TBase>(), RTTI::template get<TBase>(obj)});
                assert(deleteHandlerIt != _baseToDerivedMap.end());
                return deleteHandlerIt->second;
            }


        };

    }

}

#endif //BITSERY_EXT_POLYMORPHISM_UTILS_H
