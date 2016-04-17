#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>

#include <ndds/ndds_cpp.h>
#include "rdma_dds.h"
#include "ndds/ndds_requestreply_cpp.h"

#define THROTTLE_ENABLE

#define DOMAIN_ID 65

#define DEVICE_NAME "mlx4_0"
#define NBUFS 20
#define IB_PORT 1
#define GID_IDX -1
#define SAMPLE_COUNT 0
#define MSG_SIZE (5*1024*1024)

#define FRAGMENT1_TOPIC    "Fragment1Topic"
#define FRAGMENT2_TOPIC    "Fragment2Topic"
#define FRAGMENT3_TOPIC    "Fragment3Topic"
#define FRAGMENT4_TOPIC    "Fragment4Topic"
#define CHANNEL_INFO_TOPIC "ChannelInfoTopic"
#define WAVEFORM1_TOPIC    "Waveform1Topic"
#define WAVEFORM2_TOPIC    "Waveform2Topic"

#define FRAGMENT_TYPE      "FragmentType"
#define CHANNEL_INFO_TYPE  "ChannelInfoType"
#define WAVEFORM_TYPE      "WaveformType"

#define QC_SERVICE "QualityControlService"

#define INPUT1_PRODUCER "Inp(inp1)"
#define INPUT2_PRODUCER "Inp(inp2)"
#define INPUT3_PRODUCER "Inp(inp3)"
#define INPUT4_PRODUCER "Inp(inp4)"
#define T1_C1_PRODUCER  "T1(c-1)  "
#define T3_BS1_PRODUCER "T3(bs1)  "
#define T2_C2_PRODUCER  "T2(c-2)  "
#define A2_QC_PRODUCER  "A2(qc)   "

#define CQ_TIMEOUT_MSEC 50
#define DISC_TIMEOUT_SEC 600

std::atomic<int> sem_count(0);
#ifdef THROTTLE_ENABLE
RTIOsapiSemaphore* inp_sem = RTIOsapiSemaphore_new (RTI_OSAPI_SEMAPHORE_KIND_COUNTING, NULL);
#endif

namespace darkart
{
  struct Fragment : public RDMA_Type
  {
    pi_vector<int> frag;
    int x;

    Fragment(Allocator *alloc) 
      : frag(alloc)
    {}
  };

  struct Waveform : public RDMA_Type
  { 
    pi_vector<double> wave;
    int x;
    int trigger_index;
    double sample_rate; //in MHz

    Waveform(Allocator *alloc) :
      wave(alloc) {}
  };

  struct Channel
  { 
    static const int INVALID_CHANNEL_ID = -1;
    static const int SUMCH_ID = -2;

    int           board_id; //id of the board the channel is on
    int        channel_num; //number of the channel on the board
    int         channel_id; //global unique channel id
    pi_string        label; //helpful label for each channel
    int      trigger_count; //used to check for ID_MISMATCH
    int        sample_bits; //digitizer resolution
    double     sample_rate; //samples per microsecond
    int      trigger_index; //sample at which trigger occurred
    int             nsamps; //number of samples in the waveform
    bool         saturated; //did the signal hit the max or min range of the digitizer?

    Channel(Allocator *alloc) 
      : label(alloc) 
    {}
  };

  struct Association
  {
    struct Channel channel;
    struct Waveform waveform;

    Association(Allocator *alloc) 
     :  channel(alloc),
        waveform(alloc){}
  };

} // namespace darkart

REFLEX_ADAPT_STRUCT(
    darkart::Waveform,
    (pi_vector<double>,          wave)
    (int, x)
    (int,                 trigger_index)
    (double,                sample_rate)
    (size_t, addr)
    (size_t, rkey))

REFLEX_ADAPT_STRUCT(
    darkart::Fragment,
    (pi_vector<int>, frag)       
    (int, x)
    (size_t, addr)        
    (size_t, rkey))

REFLEX_ADAPT_STRUCT(
    darkart::Channel,
    (int,              board_id, REFLEX_KEY)
    //(int,           channel_num)
    (int,            channel_id)
    (pi_string,         label)
    //(int,         trigger_count)
    (int,           sample_bits)
    //(double,        sample_rate)
    (int,         trigger_index)
    //(int,                nsamps)
    (bool,            saturated))

REFLEX_ADAPT_STRUCT(
    darkart::Association,
    (darkart::Channel, channel, REFLEX_KEY)
    (darkart::Waveform, waveform))

void delete_entities(DDSDomainParticipant * participant)
{
  DDS_ReturnCode_t rc;
  if (participant != NULL)
  {
    rc = participant->delete_contained_entities();
    if (rc != DDS_RETCODE_OK) {
      std::cerr << "! Unable to delete participant contained entities: "
        << rc << std::endl;
    }

    rc = DDSDomainParticipantFactory::get_instance()->delete_participant(
        participant);
    if (rc != DDS_RETCODE_OK) {
      std::cerr << "! Unable to delete participant: " << rc << std::endl;
    }
  }
}

long operator - (const timeval & t1, const timeval & t2) 
{
  long start_milli = t1.tv_sec*1000 + t1.tv_usec/1000;
  long end_milli = t2.tv_sec*1000 + t2.tv_usec/1000;
  return end_milli-start_milli;
}

void tput(std::string src, 
          std::string dst,
          timeval race_start,
          const timeval &start, 
          const timeval &end,
          size_t total_iter,
          size_t lap_size,
          size_t bufsize)
{
  char host_name[128];
  gethostname(host_name, sizeof(host_name));
  long lap_diff = start - end;
  long total_diff = race_start - end;

  /*
     if (total_iter == 1000) {
     system("clear");
     }
     */
  /*

  if (total_iter % 10000 == 1000) {
    std::cout << std::string(100,'-') << std::endl;
    std::cout << "Host Name (" << host_name << ")" << std::endl;
    std::cout << "Source\t\tDestination\tIteration\tTime\t\tThroughput\tAvg. Throughput" << std::endl;
    std::cout << std::string(100,'-') << std::endl;
  }
  std::cout 
    << src << "\t"
    << dst << "\t\t" 
    << std::dec << total_iter << "\t\t"
    << lap_diff << " ms\t\t"
    << (double) lap_size*bufsize*8/1024/1024/lap_diff << " Gps\t"
    << (double) total_iter*bufsize*8/1024/1024/total_diff << " Gps\t"
    << std::endl;
    */
  std::cout 
    << host_name << " "
    << src << " "
    << dst << " "
    << "#: " << std::dec << std::setw(6) << total_iter 
    << " time: " << lap_diff << " ms"
    << " tput: "
    << std::setprecision(4) << std::fixed 
    << (double) lap_size*bufsize*8/1024/1024/1024/lap_diff*1000 << " Gbps"
    << " avg tput: "
    << std::setprecision(4) << std::fixed 
    << (double) total_iter*bufsize*8/1024/1024/1024/total_diff*1000 << " Gbps"
    << std::endl;
}           

int inp(std::string topic_name)
{
  DDS_ReturnCode_t rc;
  DDSDomainParticipant *participant = NULL;
  DDS_DynamicDataTypeProperty_t props;
  DDS_Duration_t period {1,0};

  participant = DDSDomainParticipantFactory::get_instance()->
    create_participant(
        DOMAIN_ID,
        DDS_PARTICIPANT_QOS_DEFAULT,
        NULL,   // Listener
        DDS_STATUS_MASK_NONE);

  if (participant == NULL) {
    std::cerr << "! Unable to create DDS domain participant" << std::endl;
    return -1;
  }

  BufferPool pool(MSG_SIZE, NBUFS);

  RDMA_DataWriter<darkart::Fragment> 
    dw(reflex::pub::DataWriterParams(participant).topic_name(topic_name),
       &pool, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS, DISC_TIMEOUT_SEC);

  sleep(2);

  for(int i = 0;(SAMPLE_COUNT==0) |(i < SAMPLE_COUNT);++i)
  { 
    MemRegion<darkart::Fragment> reg = dw.get_mr();
    reg.sample->frag.serialize(false);
    reg.sample->frag.reserve(255);
    reg.sample->x = i;
    for (int j=0; j<255; j++)
      reg.sample->frag.push_back(j);
#ifdef DEBUG
    std::cout << "writing " << std::dec << reg.sample->frag << std::endl;
#endif
    dw.write(std::move(reg));
  } 
  return 0;
}

int fragment_dr(DDSDomainParticipant *participant, 
                std::string topic_name, 
                RTIOsapiSemaphore *sem, 
                std::string src, 
                std::string dst)
{
  try {
    BufferPool pool(MSG_SIZE, NBUFS);

    RDMA_DataReader<darkart::Fragment> 
      dr(reflex::sub::DataReaderParams(participant).topic_name(topic_name),
         &pool, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS, CQ_TIMEOUT_MSEC, DISC_TIMEOUT_SEC);

    timeval lap_start, lap_end;

    timeval race_start;
    gettimeofday(&race_start, NULL);
    lap_start = race_start;
    int lap_size = 1000;

    for(int i = 0;(SAMPLE_COUNT==0) || (i < SAMPLE_COUNT);) 
    {   
      DDS_ReturnCode_t rc; 
      MemRegion<darkart::Fragment> region = dr.take(rc);
      RTIOsapiSemaphore_give(sem);
      ++sem_count;
#ifdef THROTTLE_ENABLE
      if (dst==T1_C1_PRODUCER)
        RTIOsapiSemaphore_take (inp_sem, NULL);
#endif
      if(rc == DDS_RETCODE_OK &&  
          region.sample.info().valid_data)
      {   
        if (i != region.sample->x)
          std::cout << "out of order!\n";
        ++i;
        if(i % 1000 == 0)
        {   
          //std::cout << region.sample->frag[254] << std::endl;
          gettimeofday(&lap_end, NULL);
          tput(src, dst, race_start, lap_start, lap_end, i, lap_size, MSG_SIZE);
          lap_start = lap_end;

          //gettimeofday(&lap_start, NULL);
        }   
        dr.underlying()->acknowledge_sample(region.sample.info());
      }   
    }   
    return 0;
  } catch(std::exception &ex) {std::cout << ex.what() << std::endl;}
}

int t1_c1()
{
  DDS_ReturnCode_t rc;
  DDSDomainParticipant *participant = NULL;

  participant = DDSDomainParticipantFactory::get_instance()->
    create_participant(
        DOMAIN_ID,
        DDS_PARTICIPANT_QOS_DEFAULT,
        NULL,   // Listener
        DDS_STATUS_MASK_NONE);

  if (participant == NULL) {
    std::cerr << "! Unable to create DDS domain participant" << std::endl;
    return -1;
  }

  RTIOsapiSemaphore* sem = RTIOsapiSemaphore_new (RTI_OSAPI_SEMAPHORE_KIND_COUNTING, NULL);
  /*
     BufferPool pool(MSG_SIZE, NBUFS);
  // Fragment DataReaders
  std::cout << "Creating consumer(T1-c1 Fragment DataReader1))\n";
  RDMA_DataReader<darkart::Fragment> 
  dr1(&pool, participant, FRAGMENT1_TOPIC, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS);
  std::cout << "Creating consumer(T1-c1 Fragment DataReader2))\n";
  RDMA_DataReader<darkart::Fragment> 
  dr2(&pool, participant, FRAGMENT2_TOPIC, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS);
  std::cout << "Creating consumer(T1-c1 Fragment DataReader3))\n";
  RDMA_DataReader<darkart::Fragment> 
  dr3(&pool, participant, FRAGMENT3_TOPIC, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS);
  */

  std::thread dr1_thread(fragment_dr, participant, FRAGMENT1_TOPIC, sem, INPUT1_PRODUCER, T1_C1_PRODUCER);
  dr1_thread.detach();
  sleep(1);

  std::thread dr2_thread(fragment_dr, participant, FRAGMENT2_TOPIC, sem, INPUT2_PRODUCER, T1_C1_PRODUCER);
  dr2_thread.detach();
  sleep(1);

  std::thread dr3_thread(fragment_dr, participant, FRAGMENT3_TOPIC, sem, INPUT3_PRODUCER, T1_C1_PRODUCER);
  dr3_thread.detach();
  sleep(1);

  BufferPool pool(MSG_SIZE, NBUFS*3);
  // Waveform DataWriter
  std::cout << "Creating producer(T1-c1 Waveform DataWriter))\n";
  RDMA_DataWriter<darkart::Waveform> 
    dw(reflex::pub::DataWriterParams(participant).topic_name(WAVEFORM1_TOPIC), 
       &pool, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS);

  timeval sem_start;
  timeval sem_end;

  int sem_prev_count = 0;

  gettimeofday(&sem_start, NULL);

  for (int i = 0; ;i++)
  {

#ifdef THROTTLE_ENABLE
    RTIOsapiSemaphore_give(inp_sem);
#endif
    RTIOsapiSemaphore_take (sem, NULL);
    --sem_count;
#ifndef THROTTLE_ENABLE
    if ((i % 1000) == 0) {
      gettimeofday(&sem_end, NULL);
      long sem_time_diff = sem_start - sem_end;
      //long sem_delta = sem_count - sem_prev_count;
      //std::cout << "Semaphore time diff:" << sem_time_diff << std::endl;
      std::cout << "Semaphore count:" << sem_count << std::endl;
      //std::cout << "sem_count per sec: " << (double)sem_delta/sem_time_diff*1000 << std::endl;
      std::cout << "sem_count per sec: " << (double)sem_count/sem_time_diff*1000 << std::endl;
      std::cout << "throughput diff per sec: " 
                << (double)sem_count/sem_time_diff*1000*MSG_SIZE*8/1024/1024/1024 
                << " Gbps" << std::endl;
      //gettimeofday(&sem_start, NULL);
      sem_prev_count = sem_count;
    }
#endif
    MemRegion<darkart::Waveform> reg = dw.get_mr();
    reg.sample->wave.serialize(false);
    reg.sample->wave.reserve(255);
    for (int j=0; j<255; j++)
      reg.sample->wave.push_back(j);
    reg.sample->x = i;
    dw.write(std::move(reg));
  }

  RTIOsapiSemaphore_delete (sem);
  return 0;
}

class QCReplierListener : public connext::SimpleReplierListener<DynamicData, DynamicData>
{
  public:
    DynamicData* on_request_available(connext::SampleRef<DynamicData> request)
    {
      if (!request.info().valid_data) {
        return NULL;
      }
      std::cout << "Replier: QC request(Channel) message received!" << std::endl;

      pi_sample<darkart::Association> reply;
      reply->channel.label = "c-2";
      reflex::write_dynamicdata(*rep_ddi_, reply.data());

      std::cout 
        << "Replier: QC reply(Association<Channel, Waveform>) message sent!" 
        << std::endl;


      return rep_ddi_;
    }
    void return_loan(DynamicData* reply)
    {
    }
    void set_sample(DynamicData* rep_ddi)
    {
      rep_ddi_ = rep_ddi;
    }
  private:
    DynamicData* rep_ddi_;
};

int qc_replier (DDSDomainParticipant *participant)
{
  DDS_DynamicDataTypeProperty_t props;

  reflex::SafeTypeCode<darkart::Channel> 
    req_stc(reflex::make_typecode<darkart::Channel>());

  std::shared_ptr<DDSDynamicDataTypeSupport>
    req_safe_typeSupport(new DDSDynamicDataTypeSupport(req_stc.get(), props));

  reflex::AutoDynamicData req_ddi(req_safe_typeSupport.get());

  reflex::SafeTypeCode<darkart::Association> 
    rep_stc(reflex::make_typecode<darkart::Association>());

  std::shared_ptr<DDSDynamicDataTypeSupport>
    rep_safe_typeSupport(new DDSDynamicDataTypeSupport(rep_stc.get(), props));

  reflex::AutoDynamicData rep_ddi(rep_safe_typeSupport.get());

  QCReplierListener *qc_replier_listener = new QCReplierListener();
  qc_replier_listener->set_sample(rep_ddi.get());

  connext::SimpleReplierParams<DynamicData, DynamicData> 
    replier_params(participant, *qc_replier_listener);
  replier_params.request_type_support(req_safe_typeSupport.get());
  replier_params.reply_type_support(rep_safe_typeSupport.get());
  replier_params.service_name(QC_SERVICE);

  connext::SimpleReplier<DynamicData, DynamicData> replier(replier_params);

  for (;;) {
    sleep(1);
  }
}

int t2_c2()
{
  DDS_ReturnCode_t rc;
  DDSDomainParticipant *participant = NULL;
  DDS_DynamicDataTypeProperty_t props;

  participant = DDSDomainParticipantFactory::get_instance()->
    create_participant(
        DOMAIN_ID,
        DDS_PARTICIPANT_QOS_DEFAULT,
        NULL,   // Listener
        DDS_STATUS_MASK_NONE);

  if (participant == NULL) {
    std::cerr << "! Unable to create DDS domain participant" << std::endl;
    return -1;
  }
  RTIOsapiSemaphore* sem = RTIOsapiSemaphore_new (RTI_OSAPI_SEMAPHORE_KIND_COUNTING, NULL);
  BufferPool pool(MSG_SIZE, NBUFS);

  // Fragment DataReader
  std::cout << "Creating consumer(T2-c2 Fragment DataReader))\n";
  std::thread dr_thread(fragment_dr, participant, FRAGMENT4_TOPIC, sem, 
                        INPUT1_PRODUCER, T2_C2_PRODUCER);
  dr_thread.detach();
  sleep(1);

  // QC Replier
  std::thread replier_thread(qc_replier, participant);

  // Waveform DataWriter
  std::cout << "Creating producer(T2-c2 Waveform DataWriter))\n";
  RDMA_DataWriter<darkart::Waveform> 
    dw(reflex::pub::DataWriterParams(participant).topic_name(WAVEFORM2_TOPIC), 
       &pool, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS, DISC_TIMEOUT_SEC);

  for (;;)
  {
    RTIOsapiSemaphore_take (sem, NULL);
    MemRegion<darkart::Waveform> reg = dw.get_mr();
    reg.sample->wave.serialize(false);
    reg.sample->wave.reserve(255);
    for (int i=0; i<255; i++)
      reg.sample->wave.push_back(i);
    dw.write(std::move(reg));
  }

  RTIOsapiSemaphore_delete (sem);

  return 0;
}


int waveform_dr(DDSDomainParticipant *participant, RTIOsapiSemaphore *sem, 
                std::string topic_name, std::string src, std::string dst)
{
  try {
    BufferPool pool(MSG_SIZE, NBUFS);

    RDMA_DataReader<darkart::Waveform> 
      dr(reflex::sub::DataReaderParams(participant).topic_name(topic_name),
         &pool, DEVICE_NAME, IB_PORT, GID_IDX, NBUFS, CQ_TIMEOUT_MSEC, DISC_TIMEOUT_SEC);

    timeval lap_start, lap_end;

    timeval race_start;
    gettimeofday(&race_start, NULL);
    lap_start = race_start;
    int lap_size = 1000;

    for(int i = 0;(SAMPLE_COUNT==0) || (i < SAMPLE_COUNT);) 
    {   
      DDS_ReturnCode_t rc; 
      MemRegion<darkart::Waveform> region = dr.take(rc);

      if(rc == DDS_RETCODE_OK &&  
         region.sample.info().valid_data)
      {   
        ++i;
        if(i % 1000 == 0)
        {   
          RTIOsapiSemaphore_give(sem);
          //std::cout << region.sample->frag[254] << std::endl;
          gettimeofday(&lap_end, NULL);
          tput(src, dst, race_start, lap_start, lap_end, i, lap_size, MSG_SIZE);
          lap_start = lap_end;
          //gettimeofday(&lap_start, NULL);
        }   
        dr.underlying()->acknowledge_sample(region.sample.info());
      }   
    }   
    return 0;
  } 
  catch(std::exception &ex) {
    std::cout << ex.what() << std::endl;
  }
}

int t3_bs1()
{
  DDS_ReturnCode_t rc;
  DDSDomainParticipant *participant = NULL;

  participant = DDSDomainParticipantFactory::get_instance()->
    create_participant(
        DOMAIN_ID,
        DDS_PARTICIPANT_QOS_DEFAULT,
        NULL,   // Listener
        DDS_STATUS_MASK_NONE);

  if (participant == NULL) {
    std::cerr << "! Unable to create DDS domain participant" << std::endl;
    return -1;
  }
  RTIOsapiSemaphore* sem = RTIOsapiSemaphore_new (RTI_OSAPI_SEMAPHORE_KIND_COUNTING, NULL);
  BufferPool pool(MSG_SIZE, NBUFS);

  std::cout << "Creating consumer(T3-bs1 Waveform DataReader)\n";
  std::thread dr_thread(waveform_dr, participant, sem, WAVEFORM1_TOPIC, T1_C1_PRODUCER, T3_BS1_PRODUCER);
  dr_thread.detach();
  sleep(1);

  for (;;)
  {
    sleep(1);
  }

  return 0;
}

int qc_requester(DDSDomainParticipant *participant, RTIOsapiSemaphore *sem)
{
  DDS_DynamicDataTypeProperty_t props;

  reflex::SafeTypeCode<darkart::Channel> 
    req_stc(reflex::make_typecode<darkart::Channel>());

  std::shared_ptr<DDSDynamicDataTypeSupport>
    req_safe_typeSupport(new DDSDynamicDataTypeSupport(req_stc.get(), props));

  reflex::AutoDynamicData req_ddi(req_safe_typeSupport.get());

  reflex::SafeTypeCode<darkart::Association> 
    rep_stc(reflex::make_typecode<darkart::Association>());

  std::shared_ptr<DDSDynamicDataTypeSupport>
    rep_safe_typeSupport(new DDSDynamicDataTypeSupport(rep_stc.get(), props));

  reflex::AutoDynamicData rep_ddi(rep_safe_typeSupport.get());

  connext::RequesterParams requester_params(participant);
  requester_params.request_type_support(req_safe_typeSupport.get());
  requester_params.reply_type_support(rep_safe_typeSupport.get());
  requester_params.service_name(QC_SERVICE);

  connext::Requester<DynamicData, DynamicData> requester(requester_params);

  pi_sample<darkart::Channel> request;
  request->label = "c-2";

  reflex::write_dynamicdata(*req_ddi.get(), request.data());

  const DDS::Duration_t MAX_WAIT = {30, 0}; 

  sleep(1);

  DDS_ExceptionCode_t ec;
  std::cout << "Requester Type" << std::endl;
  //req_stc.get()->print_IDL(2, ec);
  reflex::detail::print_IDL(req_stc.get(), 2);
  std::cout << "Replier Type" << std::endl;
  //rep_stc.get()->print_IDL(2, ec);
  reflex::detail::print_IDL(rep_stc.get(), 2);

  for (;;) {
    RTIOsapiSemaphore_take (sem, NULL);
    requester.send_request(*req_ddi.get());
    std::cout << "Requester: QC request(Channel) message sent!\n";

    connext::LoanedSamples<DynamicData> replies =
      requester.receive_replies(MAX_WAIT);

    typedef connext::LoanedSamples<DynamicData>::iterator iterator;
    for (iterator it = replies.begin(); it != replies.end(); ++it) {
      if (it->info().valid_data) {
        std::cout 
          << "Requester: QC reply(Association<Channel, Waveform>) message received!"
          << std::endl;
      }   
    }   
  }

  return 0;
}

int qc()
{
  DDS_ReturnCode_t rc;
  DDSDomainParticipant *participant = NULL;

  participant = DDSDomainParticipantFactory::get_instance()->
    create_participant(
        DOMAIN_ID,
        DDS_PARTICIPANT_QOS_DEFAULT,
        NULL,   // Listener
        DDS_STATUS_MASK_NONE);

  if (participant == NULL) {
    std::cerr << "! Unable to create DDS domain participant" << std::endl;
    return -1;
  }
  RTIOsapiSemaphore* sem = RTIOsapiSemaphore_new (RTI_OSAPI_SEMAPHORE_KIND_COUNTING, NULL);

  BufferPool pool(MSG_SIZE, NBUFS);

  std::cout << "Creating consumer(qc Waveform DataReader)\n";
  std::thread dr_thread(waveform_dr, participant, sem, WAVEFORM2_TOPIC, T1_C1_PRODUCER, T3_BS1_PRODUCER);
  dr_thread.detach();
  sleep(1);

  // Requester
  std::thread requester_thread (qc_requester, participant, sem);

  for (;;) {
    sleep(1);
  }

  return 0;
}

int main(int argc, const char **argv)
{
  try {
    if(argc <= 1){
      std::cout << "Please specify a producer (ex.inp, t1-c1, t2-c2, t3-bs1, t3-bs2, qc)\n";
    }

    if(std::string(argv[1]) == "t1-c1")
      t1_c1();
    else if(std::string(argv[1]) == "t2-c2")
      t2_c2();
    else if(std::string(argv[1]) == "t3-bs1")
      t3_bs1();
    else if(std::string(argv[1]) == "qc")
      qc();
    else if(std::string(argv[1]) == "inp1")
      inp(FRAGMENT1_TOPIC);
    else if(std::string(argv[1]) == "inp2")
      inp(FRAGMENT2_TOPIC);
    else if(std::string(argv[1]) == "inp3")
      inp(FRAGMENT3_TOPIC);
    else if(std::string(argv[1]) == "inp4")
      inp(FRAGMENT4_TOPIC);
    else
      std::cout << "Please specify a producer (ex.t1-c1, t2-c2, t3-bs1, t3-bs2)\n";
  } catch(...) {std::cout << "exception\n";}

  return 0;
}
