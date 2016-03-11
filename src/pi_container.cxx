#include <string.h>
#include <assert.h>

#include <iosfwd>
#include <stdexcept>
#include <iostream>

#include "pi_container.h"

#ifdef RTI_WIN32
  RDMA_DDS_DLL_EXPORT extern size_t default_bufsize = DEFAULT_BUFSIZE;
  RDMA_DDS_DLL_EXPORT extern size_t initial_buffers = INITIAL_BUFFERS;
#else
  size_t default_bufsize = DEFAULT_BUFSIZE;
  size_t initial_buffers = INITIAL_BUFFERS;
#endif

RDMA_DDS_DLL_EXPORT BufferPool * global_pool()
{
  static BufferPool instance(default_bufsize, initial_buffers);
  return &instance;
}

BufferPool::BufferPool(unsigned int buf_size,
                       unsigned int pool_size)
  : pool(0)
{
  REDAFastBufferPoolProperty propertyIn = REDA_FAST_BUFFER_POOL_PROPERTY_DEFAULT;
  propertyIn.growth.initial = pool_size;
  pool = REDAFastBufferPool_new(buf_size, BUFFER_ALIGNMENT, &propertyIn);
  if(!pool)
    throw std::runtime_error("Can't allocate FastBufferPool");
}  

void * BufferPool::getbuf()
{
  return REDAFastBufferPool_getBuffer(pool);
}

void BufferPool::return_buf(void * buf)
{
  REDAFastBufferPool_returnBuffer(pool, buf);
}

size_t BufferPool::size() const
{
  return REDAFastBufferPool_getBufferSize(pool);
}

BufferPool::~BufferPool()
{
  if(pool)
    REDAFastBufferPool_delete(pool);
}

size_t diff(void *p1, void *p2)
{
  if(p1 >= p2)
    return (size_t)p1 - (size_t)p2;
  else
    return (size_t)p2 - (size_t)p1;
}

Allocator::Allocator(size_t buf_sz, size_t static_size)
  : buf_size(buf_sz),
    head_offset(sizeof(Allocator)+static_size)
{}

char * Allocator::allocate(size_t static_size, 
                           size_t count, 
                           size_t alignment)
{	
  char * aligned_head = ((char *)this) + head_offset;
  
  if(count)
  {
    while(((size_t) aligned_head) % alignment) 
      ++aligned_head;

    char *new_head = aligned_head + (static_size * count);
    
    if(new_head >= (((char *)this) + buf_size))
      throw std::bad_alloc();
    else
      head_offset = diff(new_head, this);
  }
  return aligned_head;
}

Allocator * pi_string::allocator_ptr() const
{
  return (Allocator *) (((char *)this) - my_offset);
}

void pi_string::allocate(size_t size)
{
  if(size)
  {
	using std::min;
    char * data_ptr = 
      allocator_ptr()->allocate(sizeof(char), 
                                size,
                                ALIGNOF(char));
    data_offset = diff(data_ptr, allocator_ptr());
  }
}

void pi_string::assign(const char * str)
{
  if(str)
  {
    size_t l = strlen(str);
    if(l > len)
      allocate(l+1);
    else if((l==0) && (len==0))
      allocate(1);

    len = l;  
	  char *dest = (char *) begin();
  	while ((*dest++ = *str++)) { /* no-op */ }
  }
}

pi_string::pi_string(Allocator * alloc)
  : my_offset(0), 
    data_offset(0),
    len(0),
    do_ser(true)
{ 
  my_offset = diff(alloc, this);
}

pi_string::pi_string(Allocator * alloc,
                     const pi_string &ps)
  : my_offset(0),
    data_offset(0),
    len(0),
    do_ser(ps.do_ser)
{ 
  my_offset = diff(alloc, this);
  assign(ps.c_str());
}

pi_string::pi_string(const pi_string &ps)
  : my_offset(0),
    data_offset(0),
    len(0),
    do_ser(ps.do_ser)
{
  my_offset = diff(ps.allocator_ptr(), this);
  assign(ps.c_str());
}

pi_string::pi_string(Allocator * alloc,
                     const char * str)
  : my_offset(0),
    data_offset(0),
    len(0),
    do_ser(true)
{ 
  my_offset = diff(alloc, this);
  assign(str);
}

pi_string & pi_string::operator = (const char *str)
{
  assign(str);
  return *this;
}

pi_string & pi_string::operator = (const pi_string &ps)
{
  assign(ps.c_str());
  do_ser = ps.do_ser;
  return *this;
}

const char * pi_string::c_str() const
{
  return begin();
}

size_t pi_string::size() const
{
  return len;
}

char * pi_string::begin() 
{
  return data_offset? (char *) allocator_ptr()+data_offset : 0;
}

char * pi_string::end() 
{
  return begin()+len;
}

const char * pi_string::begin() const
{
  return const_cast<pi_string *>(this)->begin();
}

const char * pi_string::end() const
{
  return begin()+len;
}

char & pi_string::operator [] (size_t index)
{
  return *(begin()+index);
}

const char & pi_string::operator [] (size_t index) const
{
  return *(begin()+index);
}

void pi_string::serialize(bool s)
{
  do_ser = s;
}

bool pi_string::serialize() const
{
  return do_ser;
}

pi_string::~pi_string()
{
  my_offset = 0;
  data_offset = 0;
  len = 0;
  do_ser = 0;
}

std::ostream & operator << (std::ostream &o, const pi_string &p)
{
  if(p.c_str())
    o << p.c_str();
  return o;
}


