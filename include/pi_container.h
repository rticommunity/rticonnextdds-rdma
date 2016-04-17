#ifndef RTI_PI_CONTAINER_H
#define RTI_PI_CONTAINER_H

#include "dllexport.h"
#include "reflex.h"
#include "ndds/ndds_cpp.h"
#include "ndds/reda/reda_fastBuffer.h"

#include <type_traits>
#include <algorithm>

#define simple_alignof(T) \
  (sizeof(T) > 1 ? offsetof(align::offset<T>, x) : 1)

#define ALIGNOF(T) (min(sizeof(T), simple_alignof(T)))

#define BUFFER_ALIGNMENT 16
#define DEFAULT_BUFSIZE (1024*8)
#define INITIAL_BUFFERS 20

RDMA_DDS_DLL_EXPORT extern size_t default_bufsize;  
RDMA_DDS_DLL_EXPORT extern size_t initial_bufsize; 

namespace align {
  template <class T>
  struct offset
  {
    char c;
    T x;
  };
}

class RDMA_DDS_DLL_EXPORT pi_container 
{
  // All derived types of pi_container
  // have a copy constructor of the following form.
  // pi_container(Allocator *, const pi_container &);
};

struct RDMA_DDS_DLL_EXPORT InvalidBuf {};

struct REDAFastBufferPool;

class RDMA_DDS_DLL_EXPORT BufferPool
{
  REDAFastBufferPool * pool;

  BufferPool(const BufferPool &);
  BufferPool & operator = (const BufferPool &);

public:  
  BufferPool(unsigned int buf_size,
             unsigned int pool_size);

  size_t size() const;
  void * getbuf();
  void return_buf(void * buf);
  ~BufferPool();
};

RDMA_DDS_DLL_EXPORT BufferPool * global_pool();
RDMA_DDS_DLL_EXPORT size_t diff(void *p1, void *p2);

class RDMA_DDS_DLL_EXPORT Allocator
{
  size_t buf_size;  
  size_t head_offset;

public:
  Allocator(size_t buf_sz, size_t static_size);
  char * allocate(size_t static_size, 
                  size_t count, 
                  size_t alignment);
};

class RDMA_DDS_DLL_EXPORT pi_string : public pi_container
{
  size_t my_offset;
  size_t data_offset;
  size_t len;
  bool do_ser;

public:

  typedef char * iterator;
  typedef const char * const_iterator;
  typedef char & reference;
  typedef const char & const_reference;
  typedef char value_type;

  explicit pi_string(Allocator * alloc);
  pi_string(const pi_string &);

  pi_string(Allocator * alloc,
            const char * str);

  pi_string(Allocator * alloc,
            const pi_string &ps);

  pi_string & operator = (const char *);
  pi_string & operator = (const pi_string &);

  void allocate(size_t size);
  Allocator *allocator_ptr() const;
  void assign(const char * str);
  void allocate(const char * str);

  const char * c_str() const;
  size_t size() const;
  char * begin();
  char * end();
  const char * begin() const;
  const char * end() const;
  char & operator [] (size_t index);
  const char & operator [] (size_t index) const;

  void serialize(bool s);
  bool serialize() const;

  ~pi_string();
};


RDMA_DDS_DLL_EXPORT std::ostream & operator << (std::ostream &o, const pi_string &p);

template <class T>
class pi_vector : public pi_container
{
  size_t my_offset;
  size_t data_offset;
  size_t count;
  size_t cap;
  bool do_ser;

  template <class U>
  void construct(void *ptr, 
                 const U & val,
                 typename reflex::meta::enable_if<std::is_base_of<pi_container, U>::value>::type * = 0)
  {	
    // When U is some kind of pi_container.
    new (ptr) T(allocator_ptr(), val);
  }

  template <class U>
  void construct(void *ptr, 
                 const U & val,
                 typename reflex::meta::enable_if<!std::is_base_of<pi_container, U>::value>::type * = 0)
  {	
    // When U is something else (e.g., int)
    new (ptr) T(val);
  }

  template <class... Args>
  void emplace_back_impl(std::true_type /* pi_container */, Args&&... args)
  {
	  new (end()) T(allocator_ptr(), std::forward<Args>(args)...);
	  ++count;
  }

  template <class... Args>
  void emplace_back_impl(std::false_type /* not pi_container */, Args&&... args)
  {
	  new (end()) T(std::forward<Args>(args)...);
	  ++count;
  }

  void destroy(T *ptr)
  {	
    if(ptr)
      ptr->~T();
  }

  void destroy_all() 
  {
     for(iterator iter = begin();iter != end(); ++iter)
     {
        destroy(iter);
        --count;
     }  
  }

  void default_init(size_t n, std::true_type /* pi_container */)
  {
    for(int i = count;i < n; ++i)
    {
      // call ctor with the allocator pointer.
      push_back(value_type(allocator_ptr()));
    }
  }

  void default_init(size_t n, std::false_type /* not pi_container */)
  {
    for(int i = count;i < n; ++i)
    {
      // call default ctor.
      push_back(value_type());
    }
  }

public:

  typedef T * iterator;
  typedef const T * const_iterator;
  typedef T & reference;
  typedef const T & const_reference;
  typedef T value_type;

  explicit pi_vector(Allocator * alloc)
    : my_offset(diff(alloc, this)), 
      data_offset(0),
      count(0),
      cap(0),
      do_ser(true)
  { }

  pi_vector(Allocator * alloc, const pi_vector & piv)
    : my_offset(diff(alloc, this)), 
      data_offset(0),
      count(0),
      cap(0),
      do_ser(piv.do_ser)
  { 
    reserve(piv.size());
    std::copy(piv.begin(), piv.end(), std::back_inserter(*this));
  }

  pi_vector(const pi_vector &piv)
    : my_offset(diff(piv.allocator_ptr(), this)), 
      data_offset(0),
      count(0),
      cap(0),
      do_ser(piv.do_ser)
  {
    reserve(piv.size());
    std::copy(piv.begin(), piv.end(), std::back_inserter(*this));
  }

  pi_vector & operator = (const pi_vector &piv)
  {
    reserve(piv.size());
    std::copy(piv.begin(), piv.end(), std::back_inserter(*this));
    do_ser = piv.do_ser;
  }

  Allocator * allocator_ptr() const
  {
    return (Allocator *) (((char *)this) - my_offset);
  }

  size_t capacity() const
  {
    return cap;
  }

  void reserve(size_t n)
  {
    if(n > cap)
    {
      using std::min; // ALIGNOF uses std::min
      destroy_all();
      char * data_ptr = allocator_ptr()->allocate(sizeof(T), n, ALIGNOF(T));
      data_offset = diff(data_ptr, allocator_ptr());
      cap = n;
    }
  }

  void resize(size_t n)
  {
    reserve(n);
    default_init(n, typename std::is_base_of<pi_container, value_type>::type());
  }

  void push_back(const T &t)
  {
    construct(end(), t);
    ++count;
  }

  template <class... Args>
  void emplace_back(Args&&... args)
  {
    emplace_back_impl(typename std::is_base_of<pi_container,T>::type(),
                      std::forward<Args>(args)...);
  }

  T * begin() 
  {
    return (T *) ((char *) allocator_ptr()+data_offset);
  }

  T * end() 
  {
    return begin() + count;
  }

  const T * begin() const
  {
    return const_cast<pi_vector *>(this)->begin();
  }

  const T * end() const
  {
    return begin() + count;
  }

  size_t size() const
  {
    return count;
  }

  bool empty() const
  {
    return count==0;
  }

  T & operator [] (size_t index)
  {
    return *(begin()+index);
  }
  
  const T & operator [] (size_t index) const
  {
    return *(begin()+index);
  }

  void serialize(bool s)
  {
    do_ser = s;
  }

  bool serialize() const
  {
    return do_ser;
  }

  ~pi_vector()
  {
    destroy_all();
    my_offset = 0; 
    data_offset = 0;
    count = 0;
    cap = 0;
    do_ser = 0;
  }
};

template <class T>
typename pi_vector<T>::iterator begin(pi_vector<T> & piv)
{
  return piv.begin();
}

template <class T>
typename pi_vector<T>::const_iterator begin(const pi_vector<T> & piv)
{
  return piv.begin();
}

template <class T>
typename pi_vector<T>::iterator end(pi_vector<T> & piv)
{
  return piv.end();
}

template <class T>
typename pi_vector<T>::const_iterator end(const pi_vector<T> & piv)
{
  return piv.end();
}

template <class T>
class Sample
{
  T val;
  DDS_SampleInfo val_info;

public:

  Sample()
  { }

  Sample(T &t, DDS_SampleInfo &i)
    : val(t),
      val_info(i)
  { }

  Sample(const Sample &s)
    : val(s.data()),
      val_info(s.info())
  {}

  Sample & operator = (const Sample &s)
  {
    val = s.data();
    val_info = s.info();
    return *this;
  }

  T & data()
  {
    return val;
  }

  const T & data() const
  {
    return val;
  }

  const DDS_SampleInfo & info() const
  {
    return val_info;
  }

  DDS_SampleInfo & info()
  {
    return val_info;
  }

  T * operator -> ()
  { 
    return &val;
  }

  const T * operator -> () const
  { 
    return &val;
  }
};

template <class T>
class pi_sample
{
  BufferPool *bufpool;
  void *buf;
  DDS_SampleInfo val_info;

  T * T_position()
  {
    return (T *) ((char *) buf + sizeof(Allocator));
  }

  void init(void * buf, size_t size)
  {
    if(!buf)
      throw std::runtime_error("Unable to allocate buffer");

    new (buf) Allocator(size, sizeof(T));
    new (T_position()) T((Allocator *) buf);
  }

public:

  pi_sample(BufferPool * pool = 0)
    : bufpool(pool? pool : global_pool()),
      buf(bufpool?bufpool->getbuf():0),
      val_info()
  {
    init(buf, bufpool->size());
  }

  pi_sample(BufferPool *pool, void *buffer, size_t size)
    : bufpool(pool? pool : global_pool()),
      buf(buffer? buffer : bufpool->getbuf()),
      val_info()
  {
    init(buf, size);
  }

  pi_sample(const pi_sample & ps)
    : bufpool(ps.bufpool? ps.bufpool : global_pool()),
      buf(bufpool? bufpool->getbuf() : 0),
      val_info(ps.val_info)
  {
    init(buf, bufpool->size());
  }

  pi_sample(pi_sample && ps)
    : bufpool(ps.bufpool? ps.bufpool : global_pool()),
      buf(ps.buf? ps.buf : (bufpool? bufpool->getbuf() : 0)),
      val_info(ps.val_info)
  {
    ps.bufpool = 0;
    ps.buf = 0;
    // no init necessary
  }

  pi_sample & operator = (pi_sample ps)
  {
    ps.swap(*this);
    return *this;
  }

  void swap(pi_sample & other)
  {
    using std::swap;

    swap(other.bufpool, this->bufpool);
    swap(other.buf, this->buf);
    swap(other.val_info, this->val_info);
  }

  void reset()
  {
    bufpool = 0;
    buf = 0;
  }

  T & data()
  {
    return *T_position();
  }

  const T & data() const
  {
    return *T_position();
  }

  Allocator * allocator() const
  {
    return (Allocator *) buf;
  }

  void * raw_buf() const
  {
    return buf;
  }

  BufferPool * pool() const
  {
    return bufpool;
  }
  
  size_t buf_size() const
  {
    return bufpool->size();
  }

  const DDS_SampleInfo & info() const
  {
    return val_info;
  }

  DDS_SampleInfo & info()
  {
    return val_info;
  }

  T * operator -> ()
  { 
    return T_position();
  }

  const T * operator -> () const
  { 
    return T_position();
  }

  ~pi_sample()
  {
    if(bufpool && buf)
      bufpool->return_buf(buf);
  }
};

namespace reflex {
  namespace type_traits {

    template <>
    struct is_string<pi_string>
    {
      enum { value = true };
    };
  
    template <class T>
    struct is_container<pi_vector<T>>
    {
      enum { value = true };
    };
  
    template <class T>
    struct is_vector<pi_vector<T>>
    {
      enum { value = true };
    };
  
  } // namespace type_traits
} // namespace reflex

#endif // RTI_PI_CONTAINER_H

