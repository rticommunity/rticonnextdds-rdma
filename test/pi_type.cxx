#include "pi_type.h"
#include "tuple_reader_writer.h"
#include <memory>

void delete_entities(DDSDomainParticipant * participant);

void write_pi_type(int domain_id)
{
  DDS_ReturnCode_t         rc;
  DDSDomainParticipant *   participant = NULL;
  DDSTopic *               topic = NULL;
  DDSDataWriter *dataWriter         = NULL;
  DDSDynamicDataWriter *ddWriter    = NULL;
  DDS_DynamicDataTypeProperty_t props;
  DDS_ExceptionCode_t ex;
  DDS_Duration_t period { 0, 100*1000*1000 };
  const int SIZE = 5;

  pi_sample<PI_Shapes> sample, sample2;
  sample->color = "BLUE";
  sample->shapesize = 30;
  sample->nums.reserve(SIZE);

  int data[SIZE] = { 1, 2, 3, 4, 5 };
  std::copy(data, data+SIZE, std::back_inserter(sample->nums));
  sample->nums.serialize(false);

  const char * names[SIZE+1] = { "RTI", "Sunnyvale", "California", "", 0 };
  
  sample->vs.reserve(SIZE);
  for(const char **n_ptr = names; *n_ptr; ++n_ptr)
  {
    sample->vs.emplace_back(*n_ptr);
  }

  sample->vvs.reserve(1);
  sample->vvs.push_back(sample->vs);

  for(int i=0; i < sample->vs.size();++i)
  {
    sample->vs[i].serialize(false);
  }

  const char *topic_name = "Square";

  std::cout << "Writing " << StructName<PI_Shapes>::get()
            << " topic = " << topic_name
            << " (domain = " << domain_id << ")...\n";

  int x_max=200, y_max=200;
  int x_min=30, y_min=30;
  int x_dir=2, y_dir=2;

  int x = (rand() % 50)+x_min;
  int y = (rand() % 50)+y_min;

  participant =
    DDSDomainParticipantFactory::get_instance()->
                      create_participant(
                        domain_id,
                        DDS_PARTICIPANT_QOS_DEFAULT,
                        NULL,   // Listener
                        DDS_STATUS_MASK_NONE);

  if (participant == NULL) {
      std::cerr << "! Unable to create DDS domain participant" << std::endl;
      throw 0;
  }

  GenericDataWriter<PI_Shapes>
    shapes_writer(participant, topic_name, "Shapes");

  SafeTypeCode<DDS_TypeCode> stc(Tuple2Typecode<PI_Shapes>());

  std::shared_ptr<DDSDynamicDataTypeSupport> 
    safe_typeSupport(new DDSDynamicDataTypeSupport(stc.get(), props));

  // print idl
  print_IDL(stc.get(), 0);
  
  SafeDynamicDataInstance ddi1(safe_typeSupport.get());
  SafeDynamicDataInstance ddi2(safe_typeSupport.get());

  for(;;)
  {
    if((x >= x_max) || (x <= x_min)) 
       x_dir *= -1;
    if((y >= y_max) || (y <= y_min))
       y_dir *= -1;

    // change app data like usual.
    // tied tuple has references.
    x += x_dir;
    y += y_dir;  
  
    sample->x = x;
    sample->y = y;

    // write the tied tuple in a dynamic data instance.
    Tuple2DD(sample.data(), *ddi1.get());

    // print if you like
    // ddi1.get()->print(stdout, 2);

    // read the dynamic data instance back 
    // in a different tuple
    DD2Tuple(*ddi1.get(), sample2.data());

    // write the second tuple again in a
    // different dynamic data instance.
    Tuple2DD(sample2.data(), *ddi2.get());

    // print if you like
    // ddi2.get()->print(stdout, 2);

    // round-tripping must work!
    assert(ddi1.get()->equal(*ddi2.get()));

    // write it for fun!
    rc = shapes_writer.write(sample2.data());
    if(rc != DDS_RETCODE_OK) {
      std::cerr << "Write error = " << get_readable_retcode(rc) << std::endl;
      break;
    }
    NDDSUtility::sleep(period);
  }
  
  delete_entities(participant);
}


