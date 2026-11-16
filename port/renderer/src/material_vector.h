#pragma once
#include <memory>
#include <vector>
#include <utility>
#include <initializer_list>
namespace t2p {
inline bool g_sharedMaterialVectors=false;
// Published decoder vectors are immutable. Copies share their storage; an edit
// first takes a private copy. Freezing never mutates another published owner.
// The ordinary/off path retains std::vector's capacity and move behavior.
template<class T> class MaterialStorage {
 std::vector<T> local_;
 std::shared_ptr<const std::vector<T>> frozen_;
 const std::vector<T>& read()const{return frozen_?*frozen_:local_;}
 std::vector<T>& write(){if(frozen_){local_=*frozen_;frozen_.reset();}return local_;}
 public:
 using value_type=T;using size_type=size_t;using iterator=typename std::vector<T>::iterator;using const_iterator=typename std::vector<T>::const_iterator;
 MaterialStorage()=default;
 MaterialStorage(std::initializer_list<T> x):local_(x){}
 MaterialStorage(std::vector<T> x):local_(std::move(x)){}
 MaterialStorage(const MaterialStorage& x){if(g_sharedMaterialVectors&&x.frozen_)frozen_=x.frozen_;else local_=x.read();}
 MaterialStorage(MaterialStorage&&)=default;
 MaterialStorage& operator=(MaterialStorage&&)=default;
 MaterialStorage& operator=(const MaterialStorage& x){if(this==&x)return *this;if(g_sharedMaterialVectors&&x.frozen_){std::vector<T>().swap(local_);frozen_=x.frozen_;}else{local_=x.read();frozen_.reset();}return *this;}
 MaterialStorage& operator=(std::vector<T> x){local_=std::move(x);frozen_.reset();return *this;}
 MaterialStorage& operator=(std::initializer_list<T> x){write()=x;return *this;}
 void freeze(){if(g_sharedMaterialVectors&&!frozen_&&!local_.empty())frozen_=std::make_shared<const std::vector<T>>(std::move(local_));}
 bool immutable()const{return bool(frozen_);}
 const void* immutableIdentity()const{return frozen_.get();}
 size_t size()const{return read().size();}bool empty()const{return read().empty();}size_t capacity()const{return read().capacity();}
 const T* data()const{return read().data();}T* data(){return write().data();}
 const T& operator[](size_t n)const{return read()[n];}T& operator[](size_t n){return write()[n];}
 const T& at(size_t n)const{return read().at(n);}T& at(size_t n){return write().at(n);}
 const T& back()const{return read().back();}T& back(){return write().back();}
 const T& front()const{return read().front();}T& front(){return write().front();}
 const_iterator begin()const{return read().begin();}const_iterator end()const{return read().end();}
 const_iterator cbegin()const{return read().cbegin();}const_iterator cend()const{return read().cend();}
 iterator begin(){return write().begin();}iterator end(){return write().end();}
 void clear(){if(frozen_)frozen_.reset();else local_.clear();}
 void reserve(size_t n){write().reserve(n);}void resize(size_t n){write().resize(n);}
 void shrink_to_fit(){write().shrink_to_fit();}
 void push_back(const T& x){T stable=x;write().push_back(std::move(stable));}void push_back(T&& x){write().push_back(std::move(x));}
 template<class... A>T& emplace_back(A&&... a){return write().emplace_back(std::forward<A>(a)...);}
 template<class I>void assign(I a,I b){auto owner=frozen_;write().assign(a,b);}void assign(size_t n,const T& x){write().assign(n,x);}
 iterator insert(const_iterator p,const T& v){const auto off=p-read().begin();T stable=v;write();return local_.insert(local_.begin()+off,std::move(stable));}
 template<class I>iterator insert(const_iterator p,I a,I b){const auto off=p-read().begin();auto owner=frozen_;write();return local_.insert(local_.begin()+off,a,b);}
 iterator erase(const_iterator p){const auto off=p-read().begin();write();return local_.erase(local_.begin()+off);}
 iterator erase(const_iterator a,const_iterator b){const auto x=a-read().begin(),y=b-read().begin();write();return local_.erase(local_.begin()+x,local_.begin()+y);}
 void swap(MaterialStorage& x){local_.swap(x.local_);frozen_.swap(x.frozen_);}
 friend bool operator==(const MaterialStorage& a,const MaterialStorage& b){return a.read()==b.read();}
 friend bool operator!=(const MaterialStorage& a,const MaterialStorage& b){return !(a==b);}
};
#if defined(__3DS__) || defined(T2P_TEST_SHARED_UNIFORMS)
template<class T>using MaterialVector=MaterialStorage<T>;
#else
template<class T>using MaterialVector=std::vector<T>;
#endif
template<class T>bool sameMaterialVectorOwner(const std::vector<T>&,const std::vector<T>&){return false;}
template<class T>bool sameMaterialVectorOwner(const MaterialStorage<T>& a,const MaterialStorage<T>& b){return a.immutableIdentity()&&a.immutableIdentity()==b.immutableIdentity();}
template<class T>void freezeMaterialVector(std::vector<T>&){}
template<class T>void freezeMaterialVector(MaterialStorage<T>& x){x.freeze();}
template<class T>bool sharedMaterialVector(const std::vector<T>&){return false;}
template<class T>bool sharedMaterialVector(const MaterialStorage<T>& x){return x.immutable();}
}
