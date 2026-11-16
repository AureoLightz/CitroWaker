#pragma once
#include <memory>
#include <cstdint>
#include <vector>
#include <new>
#include <utility>
#include <cstring>
#include <algorithm>
#include <type_traits>
namespace t2p {
extern bool g_decodedTightGrowth; // experimental bounded capacity slack; set before decoder startup

#if defined(__3DS__) || defined(T2P_TEST_DECODE_NOINIT)
extern int g_decodedInitMode;
// Only the explicit decoder-overwrite resize may omit value initialization.
// Keep the allocator's no-argument construction trivial on ARM: testing a
// global flag per scalar made the previous experiment slower than std::vector.
template<class T>struct DecodedAllocator:std::allocator<T>{
 using value_type=T;using is_always_equal=std::true_type;
 template<class U>struct rebind{using other=DecodedAllocator<U>;};
 DecodedAllocator()noexcept=default;
 template<class U>DecodedAllocator(const DecodedAllocator<U>&)noexcept{}
 template<class U>void construct(U* p){
  static_assert(std::is_arithmetic<U>::value,"decoded scalars only");
  ::new(static_cast<void*>(p)) U;
#ifdef T2P_TEST_DECODE_NOINIT
  if(g_decodedInitMode==2)std::memset(p,0xA5,sizeof(U));
#endif
 }
 template<class U,class... A>void construct(U* p,A&&... a){::new(static_cast<void*>(p)) U(std::forward<A>(a)...);}
 template<class U>bool operator==(const DecodedAllocator<U>&)const noexcept{return true;}
 template<class U>bool operator!=(const DecodedAllocator<U>&)const noexcept{return false;}
};
template<class T>class DecodedVector:public std::vector<T,DecodedAllocator<T>>{
 using Base=std::vector<T,DecodedAllocator<T>>;using Alloc=DecodedAllocator<T>;
public:
 using Base::Base;using Base::operator=;using Base::emplace_back;
 DecodedVector()=default;
 explicit DecodedVector(size_t n,const Alloc& a=Alloc()):Base(a){resize(n);}
 void resize(size_t n){const size_t old=Base::size();Base::resize(n);if(n>old)std::fill(Base::begin()+old,Base::end(),T{});}
 void resize(size_t n,const T& v){Base::resize(n,v);}
 T& emplace_back(){Base::emplace_back(T{});return Base::back();}
 void resizeForOverwrite(size_t n){if(g_decodedInitMode==0)resize(n);else Base::resize(n);}
};
static_assert(sizeof(DecodedVector<uint32_t>)==sizeof(std::vector<uint32_t>),"decoded buffer metadata budget");
static_assert(std::is_nothrow_move_constructible<DecodedVector<float>>::value,"pool moves must not allocate");
#else
template<class T>using DecodedVector=std::vector<T>;
#endif

template<class T>inline void resizeDecoded(DecodedVector<T>& v,size_t n){
 if(g_decodedTightGrowth && n>v.capacity() && n<=v.max_size()){
  const size_t slack=std::max(size_t(16),n/4);
  v.reserve(slack<=v.max_size()-n?n+slack:n);
 }

#if defined(__3DS__) || defined(T2P_TEST_DECODE_NOINIT)
 v.resizeForOverwrite(n);
#else
 v.resize(n);
#endif
}
} // namespace t2p
