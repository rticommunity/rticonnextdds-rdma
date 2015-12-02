#include "rdma_test.h"
#include "tuple_reader_writer.h"
#include "rdma_dds.h"

#define MSG_SIZE (5*1024*1024)
#define NBUFS 20

struct PI_Shapes : public RDMA_Type
{
  int x, y, shapesize;
  PI_Shapes(Allocator * alloc)
  {}
};

RTI_ADAPT_STRUCT(
  PI_Shapes,
  (int, x)
  (int, y)
  (int, shapesize)
  (size_t, addr)
  (size_t, rkey)) 

timeval race_start;

long operator - (const timeval & t1, const timeval & t2)
{
  long start_milli = t1.tv_sec*1000 + t1.tv_usec/1000;
  long end_milli = t2.tv_sec*1000 + t2.tv_usec/1000;
  return end_milli-start_milli;
}

void shutdown(DDSDomainParticipant * participant);
void tput(const timeval &start, 
          const timeval &end,
          size_t total_iter,
          size_t lap_size,
          size_t bufsize)
{
  long lap_diff = start - end;
  long total_diff = race_start - end;
  std::cout << "# = " << std::dec << total_iter
            << ", time(ms) = " << lap_diff 
            << ", tput = " 
            << (double) lap_size*bufsize*8/1024/1024/lap_diff << " Gbps"
            << ", avg tput = " 
            << (double) total_iter*bufsize*8/1024/1024/total_diff << " Gbps"
            << std::endl;
}
int main(int argc, char *argv[])
{
    int domainId = 65;
    int sample_count = 1; /* infinite loop */
    bool is_pub = false;
    int ib_port = 1;
    int gid_idx = -1;
    std::string device_name = "mlx4_0";
    const char * topic_name;

    if (argc >= 2) {
      is_pub = !strcmp(argv[1],"-pub");
    }
    if (argc >= 3) {
      topic_name = argv[2];
    }
    if (argc >= 4) {
      sample_count = atoi(argv[3]);
    }

    try 
    {
      BufferPool pool(MSG_SIZE, NBUFS);

      DDSDomainParticipant *participant = 
        DDSTheParticipantFactory->create_participant(
          domainId, DDS_PARTICIPANT_QOS_DEFAULT, 
          NULL /* listener */, DDS_STATUS_MASK_NONE);
      if (participant == NULL) {
          printf("create_participant error\n");
          shutdown(participant);
          return -1;
      }
      timeval lap_start, lap_end;
      int i;
      if(is_pub)
      {
        std::cout << "Starting producer\n";
        RDMA_DataWriter<PI_Shapes> 
          dw(&pool, participant, topic_name, device_name, ib_port, gid_idx, NBUFS);
        sleep(2);
        gettimeofday(&race_start, NULL);
        for(i = 0;(sample_count==0) |(i < sample_count);++i)
        {
          MemRegion<PI_Shapes> reg = dw.get_mr();
          reg.sample->x = i;
          reg.sample->y = i+i;
          reg.sample->shapesize = 30;
#ifdef DEBUG
          std::cout << "writing " << std::dec << reg.sample->x << std::endl;
#endif
          dw.write(std::move(reg));
          //sleep(1);
        }
      }
      else
      {
        DDS_StringSeq params;
        DDS_DynamicDataTypeProperty_t props;
        SafeTypeCode<DDS_TypeCode> stc(MakeTypecode<PI_Shapes>());
        std::shared_ptr<DDSDynamicDataTypeSupport> 
          safe_typeSupport(new DDSDynamicDataTypeSupport(stc.get(), props));
        const char * type_name = "PI_Shapes"; 
        safe_typeSupport->register_type(participant, type_name);
        DDSTopic * topic =
          participant->create_topic(topic_name, 
                                    type_name, 
                                    DDS_TOPIC_QOS_DEFAULT,
                                    0,
                                    DDS_STATUS_MASK_NONE);
        std::string cf_topic_name = std::string("Filtered") + topic_name;
        DDSContentFilteredTopic* cf_topic =
          participant->create_contentfilteredtopic(
              cf_topic_name.c_str(),
              topic,
              "x < 5000 OR x >= 5000", // Just a test
              params);
        if(cf_topic)
          std::cout << "Created Content Filtered Topic\n";
        RDMA_DataReader<PI_Shapes> 
          dr(&pool, participant, cf_topic->get_name(), device_name, ib_port, gid_idx, NBUFS);
        std::cout << "Created consumer\n";
        sleep(1);
        gettimeofday(&race_start, NULL);
        lap_start = race_start;
        int lap_size = 1000;
        for(i = 0;(sample_count==0) || (i < sample_count);) 
        {
          DDS_ReturnCode_t rc;
          MemRegion<PI_Shapes> region = dr.take(rc);
          if(rc == DDS_RETCODE_OK && 
             region.sample.info().valid_data)
          {
#ifdef DEBUG
            fprintf(stdout, "received x=%d,y=%d\n", 
                region.sample->x, region.sample->y); 
#endif
            ++i;
            if(i % 1000 == 0)
            {
              gettimeofday(&lap_end, NULL);
              tput(lap_start, lap_end, i, lap_size, MSG_SIZE);
              gettimeofday(&lap_start, NULL);
            }
            dr.underlying()->acknowledge_sample(region.sample.info());
          }
          else
          {
            //fprintf(stdout, "take unsuccessful\n");
          }
        }
      }

      printf("sleeping...\n");
      sleep(15);
      printf("exiting...\n");
    }
    catch(std::exception &e) {
      std::cerr << "Exception: " << e.what() << std::endl;
    }

   return 0;
}

void shutdown(DDSDomainParticipant * participant)
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

