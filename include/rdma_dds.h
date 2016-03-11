#ifndef RTI_RDMA_DDS_H
#define RTI_RDMA_DDS_H

#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>

#include <infiniband/verbs.h>
#include "ndds/ndds_cpp.h"
#include "reflex.h"
#include "pi_container.h"

#include <string>
#include <stack>
#include <memory>
#include <sstream>

#define DEFAULT_BUFS 20
#define READ_TIMEOUT (5*10*1000*1000)
#define CQ_TIMEOUT 50
#define DISCOVERY_TIMEOUT_SEC 60 

struct RDMA_QP_Data
{
  size_t qp_num;
  size_t lid;
  unsigned char gid[16];
};

struct RDMA_Type
{
  size_t addr;
  size_t rkey; 

  RDMA_Type() 
    : addr(0), rkey(0)
  {}
};

static int modify_qp_to_init (ibv_qp *qp, int ib_port);
static int modify_qp_to_rts (struct ibv_qp *qp);
static int modify_qp_to_rtr (ibv_qp *qp, 
                             uint32_t remote_qpn, 
                             uint16_t dlid,
               	             uint8_t * dgid,
                             int ib_port,
                             int gid_idx);
bool wait_for_writers(DDSDataReader * reader,
                      DDS_OctetSeq & remote_qp_data,
                      int howmany_dw,
                      int disc_timeout_sec);
bool wait_for_readers(DDSDataWriter * writer,
                      DDS_OctetSeq & remote_qp_data,
                      int howmany_dr,
                      int disc_timeout_sec);
class rdma_resources;
int post_send (rdma_resources & res,
           DDS_UnsignedLongLong remote_addr,
           DDS_UnsignedLong rkey,
           ibv_wr_opcode opcode,
           void * buf,
           size_t size,
           ibv_mr * mr);
int poll_completion (rdma_resources &res, unsigned  timeout_msec);

template <class PI>
class BufferMan;

template <class PI>
struct MemRegion
{
  pi_sample<PI> sample;
  ibv_mr * mr;
  BufferMan<PI> * bufman;

  MemRegion(const MemRegion &) = delete;
  MemRegion& operator =(const MemRegion &) = delete;

  MemRegion(MemRegion &&);
  MemRegion & operator = (MemRegion &&);

  explicit MemRegion(BufferPool *pool, 
                     BufferMan<PI> & bm);

  MemRegion(BufferPool *pool, BufferMan<PI> & bm, 
            void *buf, size_t size, ibv_mr *m);

  void reset();
  
  ~MemRegion();
};

template <class T>
class RDMA_DataWriter;

template <class PI>
class AckReceiver : public DDSDataWriterListener
{
    RDMA_DataWriter<PI> * dw;
public:
    AckReceiver(RDMA_DataWriter<PI> *w);
    virtual void on_publication_matched 
       (DDSDataWriter *writer,
        const DDS_PublicationMatchedStatus &status);
    
    virtual void on_sample_removed(
      DDSDataWriter*,
      const DDS_Cookie_t &) override;
};

struct rdma_resources
{
    std::string device_name;
    int ib_port;
    int gid_idx;
    struct ibv_device_attr device_attr;
    struct ibv_port_attr port_attr;     /* IB port attributes */
    struct RDMA_QP_Data remote_props;   /* values to connect to remote side */
    ibv_context *ib_ctx;   /* device handle */
    ibv_pd *pd;            /* PD handle */
    ibv_cq *cq;            /* CQ handle */
    ibv_qp *qp;            /* QP handle */
    union ibv_gid my_gid;
    void * post_receive_buf;
    ibv_mr * post_receive_mr;
    size_t ncq;

    rdma_resources (const std::string & device_name,
                    int ib_port,
                    int gid_idx);
    int create (std::string device_name,
                int ib_port,
                int gid_idx);
    int init_qp(RDMA_QP_Data & local_data);
    int modify_qp ();
    int post_receive();
    int register_region(void * buf, size_t size, ibv_mr *& mr);
    ~rdma_resources();
};

class RDMA_EndPoint
{
public:
  rdma_resources res;
  RDMA_QP_Data local_qp_data;

  RDMA_EndPoint(const std::string & device_name, 
                int ib_port, 
                int gid_idx);

  void init();
  virtual int create_dds_entities(const DDS_OctetSeq & local_qp_data) = 0;
  virtual bool wait_for_peers(DDS_OctetSeq & remote_qp_data) = 0;

  virtual ~RDMA_EndPoint();
};

template <class PI>
struct BufferMan : public RDMA_EndPoint
{
  BufferPool *bufpool;
  std::stack<MemRegion<PI>> avail_buf;   
  size_t nbuf;
  RTIOsapiSemaphore *buf_mutex;

  BufferMan(BufferPool * pool,
            const std::string & device_name, 
            int ib_port, 
            int gid_idx,
            size_t n)
    : RDMA_EndPoint(device_name, ib_port, gid_idx),
      bufpool(pool),
      nbuf(n),
      buf_mutex(RTIOsapiSemaphore_new(RTI_OSAPI_SEMAPHORE_KIND_MUTEX, 
                                      NULL))
  {}

  void register_buffers();
  MemRegion<PI> get_mr();
  void return_mr(MemRegion<PI> && mr);
};

template <class PI>
class RDMA_DataReader : public BufferMan<PI>
{
  reflex::sub::DataReaderParams params_;
  std::shared_ptr<reflex::sub::DataReader<PI>> dr;
  DDSReadCondition * cond; 
  DDSWaitSet waitset;
  int poll_timeout_ms;
  int disc_timeout_sec;
  std::vector<reflex::sub::Sample<PI>> data_seq;

  DDS_ReturnCode_t read_meta(PI &data, DDS_SampleInfo &, int);

public:
  RDMA_DataReader(const reflex::sub::DataReaderParams & params,
                  BufferPool * pool,
                  std::string device_name, 
                  int ib_port, 
                  int gid_idx, 
                  int n = DEFAULT_BUFS,
                  int cq_timeout = CQ_TIMEOUT,
                  int disc_timeout_s = DISCOVERY_TIMEOUT_SEC)
    : BufferMan<PI>(pool, device_name, ib_port, gid_idx, n),
      params_(params),
      cond(0),
      poll_timeout_ms(cq_timeout),
      disc_timeout_sec(disc_timeout_s)
  {
    try {
      if(!params.domain_participant())
        throw std::logic_error("RDMA_DataReader: NULL domain participant");
      BufferMan<PI>::register_buffers();
      RDMA_EndPoint::init();    
    }
    catch(...)
    {
      if(cond)
      {
        waitset.detach_condition(cond);
        dr->underlying()->delete_readcondition(cond);
        throw;
      }
    }
  }

  virtual int create_dds_entities(const DDS_OctetSeq & local_qp_data);
  virtual bool wait_for_peers(DDS_OctetSeq & remote_qp_data);

  DDSDataReader * underlying() const;
  MemRegion<PI> take(DDS_ReturnCode_t &);
  ~RDMA_DataReader();
};

template <class PI>
class RDMA_DataWriter : public BufferMan<PI>
{
  reflex::pub::DataWriterParams params_;
  std::shared_ptr<reflex::pub::DataWriter<PI>> dw;
  AckReceiver<PI> ar;
  int disc_timeout_sec;
public:
  RDMA_DataWriter(const reflex::pub::DataWriterParams & params,
                  BufferPool * pool,
                  std::string device_name, 
                  int ib_port, 
                  int gid_idx, 
                  int n = 10,
                  int disc_timeout = DISCOVERY_TIMEOUT_SEC)
    : BufferMan<PI>(pool, device_name, ib_port, gid_idx, n),
      params_(params),
      ar(this),
      disc_timeout_sec(disc_timeout)
  { 
    if(!params.domain_participant())
    {
      std::cerr << "logic_error\n";
      throw std::logic_error("RDMA_DataWriter: NULL domain participant");
    }
    BufferMan<PI>::register_buffers();
    RDMA_EndPoint::init();    
  }

  virtual int create_dds_entities(const DDS_OctetSeq & local_qp_data);
  virtual bool wait_for_peers(DDS_OctetSeq & remote_qp_data);

  int write(MemRegion<PI> &&);
  ~RDMA_DataWriter();
};

template <class PI>
DDS_ReturnCode_t RDMA_DataReader<PI>::read_meta(PI &data, 
                                                DDS_SampleInfo &info, 
                                                int)
{
  //std::cout << "inside read meta\n";
  DDS_ReturnCode_t rc;
  DDS_Duration_t MAX_WAIT = {0,READ_TIMEOUT};

  DDSConditionSeq active_conditions;
  rc = waitset.wait(active_conditions, MAX_WAIT);
  if(rc != DDS_RETCODE_OK)
  {
    if(rc != DDS_RETCODE_TIMEOUT)
      std::cout << "waitset.wait failed\n";
  }
  else 
  {
    if(active_conditions[0] == cond) 
    {
      rc = dr->take_w_condition(this->data_seq, 1, cond);
      switch(rc)
      {
        case DDS_RETCODE_OK:
          data = this->data_seq[0].data();
          info = this->data_seq[0].info();
          break;
        case DDS_RETCODE_PRECONDITION_NOT_MET:
          std::cout << "PRECONDITION NOT MET\n";
          break;
        case DDS_RETCODE_NO_DATA:
          std::cout << "RETCODE_NO_DATA\n";
          break;
        case DDS_RETCODE_NOT_ENABLED:
          std::cout << "RETCODE_NOT_ENABLED\n";
          break;
        default:
          std::cout << "else\n";
          break;
      }
    }
  }
  return rc;
}

template <class PI>
MemRegion<PI> RDMA_DataReader<PI>::take(DDS_ReturnCode_t & rc)
{
  //std::cout << "before get_mr\n";
  MemRegion<PI> region = BufferMan<PI>::get_mr();
  
  rc = read_meta(region.sample.data(), 
                 region.sample.info(), 1);

  if(rc != DDS_RETCODE_OK)
  {
    return std::move(region);
  }
  else
  {
    if(region.sample.info().valid_data) 
    {
      RDMA_Type remote_data = region.sample.data();
      //std::cout << std::hex << "buf=" << (void *) region.sample->addr;
      //std::cout << std::hex << " rkey="  << (void *) region.sample->rkey << std::endl;
      if (post_send(BufferMan<PI>::res,
                    remote_data.addr, 
                    remote_data.rkey, 
                    IBV_WR_RDMA_READ,
                    region.sample.raw_buf(),
                    region.sample.buf_size(),
                    region.mr)) 
      {
        fprintf (stderr, "failed to post RR\n");
        rc = DDS_RETCODE_ERROR;
      }
      else 
      {
        if(0 != poll_completion(BufferMan<PI>::res, poll_timeout_ms))
          rc = DDS_RETCODE_TIMEOUT;
        else
        {
          rc = DDS_RETCODE_OK;
          return std::move(region);
        }
      }
    }
    else
    {
      std::cout << "DDS_RETCODE_NO_DATA\n";
      rc = DDS_RETCODE_NO_DATA;
    }
  }
  return std::move(region);
}

template <class PI>
int RDMA_DataWriter<PI>::write(MemRegion<PI> && mr)
{
  int rc = 0;
  DDS_WriteParams_t params;

  //sprintf((char *) mr.buf, "%s pid=%d sample=%d", MSG, getppid(), i);
  mr.sample->addr = (uintptr_t) mr.sample.raw_buf();
  mr.sample->rkey = mr.mr->rkey;
  
  std::stringstream stream;
  stream << std::hex 
         << (uint64_t) mr.sample.pool()    << " " 
         << (uint64_t) mr.sample.raw_buf() << " " 
         << (uint64_t) mr.sample.buf_size()    << " " 
         << (uint64_t) mr.mr;
  std::string str_mr = stream.str();
  size_t len = str_mr.size() + 1;
  params.cookie.value.ensure_length(len,len);
  std::copy(str_mr.begin(), str_mr.end(), 
            params.cookie.value.get_contiguous_buffer());
#ifdef VERBOSE 
  std::cout << std::hex << "buf=" << (void *) mr.sample->addr
            << std::hex << "rkey="  << (void *) mr.sample->rkey << std::endl;
#endif
  rc = dw->write_w_params(mr.sample.data(), params);
  if(rc != DDS_RETCODE_OK) {
    std::cout << "write_w_params failed\n";
  }
  mr.reset();

  return 0;
}

template <class PI>
int RDMA_DataWriter<PI>::create_dds_entities(const DDS_OctetSeq & local_qp_data)
{
  DDS_ReturnCode_t retcode;

  DDS_DataWriterQos  dw_qos;
  params_.domain_participant()->get_default_datawriter_qos(dw_qos);
  dw_qos.user_data.value = local_qp_data;

  dw = std::make_shared<reflex::pub::DataWriter<PI>>(
         params_.datawriter_qos(dw_qos)
                .listener(&ar)
                .listener_statusmask(DDS_STATUS_MASK_ALL | 
                                     DDS_DATA_WRITER_SAMPLE_REMOVED_STATUS));
  return 0;
}

template <class PI>
int RDMA_DataReader<PI>::create_dds_entities(const DDS_OctetSeq & local_qp_data)
{
  DDS_DataReaderQos dr_qos;
  params_.domain_participant()->get_default_datareader_qos(dr_qos);
  dr_qos.user_data.value = local_qp_data;

  dr = std::make_shared<reflex::sub::DataReader<PI>>(
        params_.datareader_qos(dr_qos));
  cond = dr->underlying()->create_readcondition(DDS_NOT_READ_SAMPLE_STATE,
                                                DDS_ANY_VIEW_STATE,
                                                DDS_ANY_INSTANCE_STATE);

  if(waitset.attach_condition(cond) != DDS_RETCODE_OK)
    std::cout << "Cannot attach condition\n";
  
  return 0;
}

template <class PI>
DDSDataReader * RDMA_DataReader<PI>::underlying() const
{
  return dr->underlying();
}

template <class PI>
RDMA_DataReader<PI>::~RDMA_DataReader()
{
  if(cond)
  {
    waitset.detach_condition(cond);
    dr->underlying()->delete_readcondition(cond);
  }
}

template <class PI>
RDMA_DataWriter<PI>::~RDMA_DataWriter()
{
  dw->underlying()->set_listener(0);
}

template <class PI>
void BufferMan<PI>::register_buffers()
{
  RTIOsapiSemaphore_take(buf_mutex, NULL);
  for(int i=0;i < nbuf;++i)
  {
    MemRegion<PI> region(bufpool, *this);

    res.register_region(region.sample.raw_buf(), 
                        region.sample.buf_size(), 
                        region.mr);
    avail_buf.push(std::move(region));
  }
  RTIOsapiSemaphore_give(buf_mutex);
}

template <class PI>
MemRegion<PI> BufferMan<PI>::get_mr()
{
 RTIOsapiSemaphore_take(buf_mutex, NULL);
 //std::cout << std::dec << "avail_buf= " << avail_buf.size() << std::endl;

 for(;;)
 {
   if(avail_buf.empty())
   {
     RTIOsapiSemaphore_give(buf_mutex);
     usleep(100);
     RTIOsapiSemaphore_take(buf_mutex, NULL);
   }
   else
     break;
 }
 MemRegion<PI> ret = std::move(avail_buf.top());
 avail_buf.pop();
 RTIOsapiSemaphore_give(buf_mutex);
 return std::move(ret);
}

template <class PI>
void BufferMan<PI>::return_mr(MemRegion<PI> && mr)
{
 RTIOsapiSemaphore_take(buf_mutex, NULL);
 avail_buf.push(std::move(mr));
 RTIOsapiSemaphore_give(buf_mutex);
}

template <class PI>
bool RDMA_DataReader<PI>::wait_for_peers(DDS_OctetSeq & remote_qp_data)
{
  return wait_for_writers(dr->underlying(), remote_qp_data, 1, disc_timeout_sec);
}

template <class PI>
bool RDMA_DataWriter<PI>::wait_for_peers(DDS_OctetSeq & remote_qp_data)
{
  return wait_for_readers(dw->underlying(), remote_qp_data, 1, disc_timeout_sec);
}

template <class PI>
AckReceiver<PI>::AckReceiver(RDMA_DataWriter<PI> *w)
  : dw(w)
{}

template <class PI>
void AckReceiver<PI>::on_publication_matched (DDSDataWriter *writer,
                                          const DDS_PublicationMatchedStatus &status)
{
  std::cout << "on_publication_matched\n";
}

template <class PI>
void AckReceiver<PI>::on_sample_removed(
  DDSDataWriter*,
  const DDS_Cookie_t & cookie)
{
//#ifdef VERBOSE 
  std::cout << "sample removed\n";
//#endif
  BufferPool * bufpool;
  void * raw_buf;
  ibv_mr * mr;
  size_t size;
  
  const char *begin = (const char *) cookie.value.get_contiguous_buffer();
  const char * end = begin + cookie.value.length();
  std::string str_mr(begin, end);
  std::stringstream stream(str_mr);
  stream >> std::hex >> (uint64_t &) bufpool
                     >> (uint64_t &) raw_buf 
                     >> (uint64_t &) size
                     >> (uint64_t &) mr;
  //std::cout << std::hex << "buf=" << (void *) mr.buf
  //          << std::hex << " mr="  << (void *) mr.mr<< std::endl;
  dw->return_mr(MemRegion<PI>(bufpool, *dw, raw_buf, size, mr));
}

template <class PI>
MemRegion<PI>::MemRegion(BufferPool *pool, 
                         BufferMan<PI> & bm)
  : sample(pool),
    mr(0),
    bufman(&bm)
{}

template <class PI>
MemRegion<PI>::MemRegion(BufferPool *pool, BufferMan<PI> & bm, 
          void *buf, size_t size, ibv_mr *m)
  : sample(pool, buf, size),
    mr(m),
    bufman(&bm)
{}

template <class PI>
void MemRegion<PI>::reset()
{
  bufman = nullptr;
  sample.reset();
}

template <class PI>
MemRegion<PI>::~MemRegion()
{
  if(bufman)
    bufman->return_mr(std::move(*this));
}

template <class PI>
MemRegion<PI>::MemRegion(MemRegion<PI> && reg)
  : sample(std::move(reg.sample)),
    mr(reg.mr),
    bufman(reg.bufman)
{
  reg.bufman = 0;
}

template <class PI>
MemRegion<PI> & MemRegion<PI>::operator = (MemRegion<PI> && reg)
{
  sample = std::move(reg.sample);
  mr = reg.mr;
  bufman = reg.bufman;
  reg.bufman = 0;

  return *this;
}

#endif // RTI_RDMA_DDS_H

