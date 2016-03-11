#include "rdma_dds.h"

DDS_OctetSeq to_octets(const RDMA_QP_Data & data)
{
  DDS_OctetSeq octets;
  std::stringstream stream;
  stream << std::hex 
         << data.qp_num << " "
         << data.lid;
  std::string str = stream.str();
  octets.ensure_length(str.size()+1, str.size()+1);
  unsigned int i=0;
  for(i=0; i < str.size();++i)
  {
    octets[i] = str[i];
  }
  octets[i]=0;
  return octets;
}

RDMA_QP_Data to_rdma_control(const DDS_OctetSeq & data)
{
  if(data.get_contiguous_buffer()==0)
    std::cerr << "null buffer\n";
  std::string str((char *) data.get_contiguous_buffer());
  std::stringstream stream(str);
  RDMA_QP_Data ret = {0};

  stream >> std::hex 
         >> ret.qp_num
         >> ret.lid;

  return ret;
}

RDMA_EndPoint::RDMA_EndPoint(const std::string & device_name, 
                             int ib_port, 
                             int gid_idx)
  : res(device_name, ib_port, gid_idx)
{ }

void RDMA_EndPoint::init()
{
  DDS_OctetSeq remote_qp_octets, local_qp_octets;
  
  if(res.init_qp(local_qp_data) !=0)
    fprintf(stderr, "init_qp failed.\n");

  local_qp_octets = to_octets(local_qp_data);
  create_dds_entities(local_qp_octets);

  bool disc_complete = wait_for_peers(remote_qp_octets);

  if(!disc_complete) {
    fprintf (stderr, "failed to finish discovery\n");
    throw std::runtime_error("discovery did not finish");
  }
  res.remote_props = to_rdma_control(remote_qp_octets);
  res.modify_qp();
}

rdma_resources::rdma_resources (const std::string & name,
                                int port,
                                int gid)
      : device_name(name), ib_port(port), gid_idx(gid),
        ib_ctx(0), pd(0), cq(0), qp(0), 
        post_receive_buf(0), post_receive_mr(0),
        ncq(10)
    {
      memset (&device_attr, 0, sizeof(ibv_device_attr));
      memset (&port_attr, 0, sizeof(ibv_port_attr));
      memset (&remote_props, 0, sizeof(RDMA_QP_Data));
      memset (&my_gid, 0, sizeof(ibv_gid));

      if(create(device_name, ib_port, gid_idx) != 0)
        throw std::runtime_error("rdma resource creation failed.");
    }

    int rdma_resources::create (std::string device_name,
                                 int ib_port,
                                 int gid_idx)
    {
      struct ibv_qp_init_attr qp_init_attr = {0};
      struct ibv_device *ib_dev = NULL;
      int i = 0;
      int cq_size = 0;
      int num_devices = 0;

      /* get device names in the system */
      std::shared_ptr<ibv_device *> safe_dev_list
        (ibv_get_device_list (&num_devices), ibv_free_device_list);

      if (!safe_dev_list) {
        std::cerr << "failed to get IB devices list\n";
        return 1;
      }

      /* if there isn't any IB device in host */
      if (!num_devices)
      {
        fprintf (stderr, "found %d device(s)\n", num_devices);
        return 1;
      }
      fprintf (stdout, "found %d device(s)\n", num_devices);

      /* search for the specific device we want to work with */
      for (i = 0; i < num_devices; i++)
      {
        if (device_name.empty())
        {
          device_name = ibv_get_device_name (safe_dev_list.get()[i]);
          fprintf (stdout,
                   "device not specified, using first one found: %s\n",
                   device_name.c_str());
        }
        if (device_name == ibv_get_device_name (safe_dev_list.get()[i]))
        {
          ib_dev = safe_dev_list.get()[i];
          break;
        }
      }
      
      /* if the device wasn't found in host */
      if (!ib_dev) {
        fprintf (stderr, "IB device %s wasn't found\n", device_name.c_str());
        return  1;
      }
      
      /* get device handle */
      ib_ctx = ibv_open_device (ib_dev);
      if (!ib_ctx) {
          fprintf (stderr, "failed to open device %s\n", device_name.c_str());
          return 1;
        }
      ib_dev = NULL;
    
      /* query port properties */
      if (ibv_query_port (ib_ctx, ib_port, &port_attr)) {
          fprintf (stderr, "ibv_query_port on port %u failed\n", ib_port);
          return 1;
        }
  
      /* allocate Protection Domain */
      pd = ibv_alloc_pd (ib_ctx);
      if (!pd) {
          fprintf (stderr, "ibv_alloc_pd failed\n");
          return 1;
        }
    
      /* each side will send only one WR, so Completion Queue with 1 entry is enough */
      cq_size = ncq;
      cq = ibv_create_cq (ib_ctx, cq_size, NULL, NULL, 0);
      if (!cq) {
          fprintf (stderr, "failed to create CQ with %u entries\n", cq_size);
          return 1;
        }
    
      /* create the Queue Pair */
      memset (&qp_init_attr, 0, sizeof (qp_init_attr));
      qp_init_attr.qp_type = IBV_QPT_RC;
      qp_init_attr.sq_sig_all = 1;
      qp_init_attr.send_cq = cq;
      qp_init_attr.recv_cq = cq;
      qp_init_attr.cap.max_send_wr = ncq;
      qp_init_attr.cap.max_recv_wr = ncq;
      qp_init_attr.cap.max_send_sge = 1;
      qp_init_attr.cap.max_recv_sge = 1;
      
      qp = ibv_create_qp (pd, &qp_init_attr);
      if (!qp) {
          fprintf (stderr, "failed to create QP\n");
          return 1;
      }
      fprintf (stdout, "QP was created, QP number=0x%x\n", qp->qp_num);

      return 0;
   }

    rdma_resources::~rdma_resources()
    {
      if(post_receive_mr) {
        ibv_dereg_mr(post_receive_mr);
        post_receive_mr = NULL;
        free(post_receive_buf);
      }
      if (qp) {
        ibv_destroy_qp (qp);
        qp = NULL;
      }
      if (cq) {
          ibv_destroy_cq (cq);
          cq = NULL;
      }
      if (pd) {
          ibv_dealloc_pd (pd);
          pd = NULL;
      }
      if (ib_ctx) {
          ibv_close_device (ib_ctx);
          ib_ctx = NULL;
      }
    }
    int rdma_resources::init_qp(RDMA_QP_Data & local_data)
    {
      int rc;
      
      if(gid_idx >= 0)
        {
          rc =
            ibv_query_gid (ib_ctx, ib_port, gid_idx, &my_gid);
          if (rc)
            {
              fprintf (stderr, "could not get gid for port %d, index %d\n",
                       ib_port, gid_idx);
              return rc;
            }
        }
      else
        memset (&my_gid, 0, sizeof(ibv_gid));
        
      /* modify the QP to init */
      rc = modify_qp_to_init (qp, ib_port);
      if (rc) {
          fprintf (stderr, "change QP state to INIT failed\n");
          return 1;
      }

        /* let the client post RR to be prepared for incoming messages */
      if (post_receive()) {
        fprintf (stderr, "failed to post RR\n");
        return 1;
      }
      
      local_data.qp_num = qp->qp_num;
      local_data.lid = port_attr.lid;
      memcpy (local_data.gid, &my_gid, 16);
      
      return 0;
    }

    int rdma_resources::post_receive()
    {
      struct ibv_recv_wr rr;
      struct ibv_sge sge;
      struct ibv_recv_wr *bad_wr;
      int rc;
      ibv_mr * mr;
      
      size_t post_receive_buf_size = 100;
      post_receive_buf = malloc(post_receive_buf_size);
      register_region(post_receive_buf, post_receive_buf_size, mr);

      /* prepare the scatter/gather entry */
      memset (&sge, 0, sizeof (sge));
      
      sge.addr = (uintptr_t) post_receive_buf;
      sge.length = post_receive_buf_size;
      sge.lkey = mr->lkey;
      /* prepare the receive work request */
      memset (&rr, 0, sizeof (rr));
      rr.next = NULL;
      rr.wr_id = 0;
      rr.sg_list = &sge;
      rr.num_sge = 1;
      /* post the Receive Request to the RQ */
      rc = ibv_post_recv (qp, &rr, &bad_wr);
      if (rc)
        fprintf (stderr, "failed to post RR\n");
      else
        fprintf (stdout, "Receive Request was posted\n");
      return rc;
    }
    
    int rdma_resources::register_region(void * buf, size_t size, ibv_mr *& mr)
    {
      /* register the memory buffer */
      int mr_flags = IBV_ACCESS_LOCAL_WRITE  | 
                     IBV_ACCESS_REMOTE_READ  |
                     IBV_ACCESS_REMOTE_WRITE;

      mr = ibv_reg_mr (pd, buf, size, mr_flags);
      if (!mr)
      {
        char buf[512];
        sprintf (buf, "ibv_reg_mr failed with mr_flags=0x%x\n", 
                 mr_flags);
        throw std::runtime_error(buf);
      }
      fprintf (stdout,
               "MR was registered with addr=%p,"
               "lkey=0x%x, rkey=0x%x, flags=0x%x\n",
               buf, mr->lkey, mr->rkey, mr_flags);
      return 0;
    }

    int rdma_resources::modify_qp ()
    {
      int rc;
      /* modify the QP to RTR */
      rc =
        modify_qp_to_rtr (qp, 
                          remote_props.qp_num, 
                          remote_props.lid,
                          remote_props.gid,
                          ib_port,
                          gid_idx);

      if (rc) {
          fprintf (stderr, "failed to modify QP state to RTR\n");
          return 1;
        }
      else
          fprintf (stderr, "Modified QP state to RTR\n");

      rc = modify_qp_to_rts (qp);
      if (rc) {
          fprintf (stderr, "failed to modify QP state to RTS\n");
          return 1;
        }
      fprintf (stdout, "QP state was change to RTS\n");
      
      /* save the remote side attributes, we will need it for the post SR */
      return rc;
    }

bool wait_for_readers(DDSDataWriter * writer,
                      DDS_OctetSeq & remote_qp_data,
                      int howmany_dr,
                      int timeout_sec)
{
    DDS_InstanceHandleSeq sub_handle_seq;
    DDS_SubscriptionBuiltinTopicData sub_builtin;
    unsigned int wait_ms_per_iter = 100;
    DDS_Duration_t wait_period = {0, 1000*1000*wait_ms_per_iter};
    int iter = timeout_sec*1000/wait_ms_per_iter;

    std::cout << "wait_for_readers: Waiting for discovery....\n"; 
    for(int count = 0; count <= iter; ++count)
    {
      writer->get_matched_subscriptions(sub_handle_seq);
      if(sub_handle_seq.length() >= howmany_dr) {
        writer->get_matched_subscription_data(sub_builtin, 
                                              sub_handle_seq[0]);
        remote_qp_data = sub_builtin.user_data.value;
        return true;
      }
      printf("waiting for readers...\n");
      NDDSUtility::sleep(wait_period);
    }
    std::cerr << "Discovery did not finish\n";
    return false;
}

bool wait_for_writers(DDSDataReader * reader,
                      DDS_OctetSeq & remote_qp_data,
                      int howmany_dw,
                      int timeout_sec)
{
    DDS_InstanceHandleSeq pub_handle_seq;
    DDS_PublicationBuiltinTopicData pub_builtin;
    unsigned int wait_ms_per_iter = 100;
    DDS_Duration_t wait_period = {0, 1000*1000*wait_ms_per_iter};
    int iter = timeout_sec*1000/wait_ms_per_iter;

    std::cout << "wait_for_writers: Waiting for discovery....\n"; 
    for(int count = 0; count <= iter; ++count)
    {
      reader->get_matched_publications(pub_handle_seq);
      if(pub_handle_seq.length() >= howmany_dw) {
        reader->get_matched_publication_data(pub_builtin, 
                                             pub_handle_seq[0]);
        remote_qp_data = pub_builtin.user_data.value;
        return true;
      }
      printf("waiting for writers...\n");
      NDDSUtility::sleep(wait_period);
    }
    std::cerr << "Discovery did not finish\n";
    return false;
}

static int
modify_qp_to_init (ibv_qp *qp, int ib_port)
{
  ibv_qp_attr attr;
  int flags;
  int rc;

  memset(&attr, 0, sizeof(ibv_qp_attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = ib_port;
  attr.pkey_index = 0;
  attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | 
                         IBV_ACCESS_REMOTE_READ |
                         IBV_ACCESS_REMOTE_WRITE;
  flags = IBV_QP_STATE      | 
          IBV_QP_PKEY_INDEX | 
          IBV_QP_PORT       | 
          IBV_QP_ACCESS_FLAGS;
          
  rc = ibv_modify_qp (qp, &attr, flags);
  if (rc)
    fprintf (stderr, "failed to modify QP state to INIT\n");
  return rc;
}

static int
modify_qp_to_rtr (ibv_qp *qp, 
                  uint32_t remote_qpn, 
                  uint16_t dlid,
		  uint8_t * dgid,
                  int ib_port,
                  int gid_idx)
{
  ibv_qp_attr attr;
  int flags;
  int rc;

  memset(&attr, 0, sizeof(ibv_qp_attr));
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_2048;
  attr.dest_qp_num = remote_qpn;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 0x12;
  attr.ah_attr.is_global = 0;
  attr.ah_attr.dlid = dlid;
  attr.ah_attr.sl = 0;
  attr.ah_attr.src_path_bits = 0;
  attr.ah_attr.port_num = ib_port;
  if (gid_idx >= 0)
    {
      attr.ah_attr.is_global = 1;
      attr.ah_attr.port_num = 1;
      memcpy (&attr.ah_attr.grh.dgid, dgid, 16);
      attr.ah_attr.grh.flow_label = 0;
      attr.ah_attr.grh.hop_limit = 1;
      attr.ah_attr.grh.sgid_index = gid_idx;
      attr.ah_attr.grh.traffic_class = 0;
    }
  flags = IBV_QP_STATE              | 
          IBV_QP_AV                 | 
          IBV_QP_PATH_MTU           |  
          IBV_QP_DEST_QPN           |
          IBV_QP_RQ_PSN             | 
          IBV_QP_MAX_DEST_RD_ATOMIC | 
          IBV_QP_MIN_RNR_TIMER;
          
  rc = ibv_modify_qp (qp, &attr, flags);
  if (rc)
    fprintf (stderr, "failed to modify QP state to RTR\n");
  return rc;
}

static int modify_qp_to_rts (struct ibv_qp *qp)
{
  ibv_qp_attr attr;
  int flags;
  int rc;

  memset(&attr, 0, sizeof(ibv_qp_attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 0x12;
  attr.retry_cnt = 6;
  attr.rnr_retry = 0;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;

flags = IBV_QP_STATE     | 
        IBV_QP_TIMEOUT   | 
        IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | 
        IBV_QP_SQ_PSN    | 
        IBV_QP_MAX_QP_RD_ATOMIC;
        
  rc = ibv_modify_qp (qp, &attr, flags);
  if (rc)
    fprintf (stderr, "failed to modify QP state to RTS\n");
  return rc;
}

int poll_completion (rdma_resources &res, unsigned  timeout_msec)
{
  struct ibv_wc wc = {0};
  unsigned long start_time_msec;
  unsigned long cur_time_msec;
  struct timeval cur_time;
  int poll_result;
  int rc = 0;
/* poll the completion for a while before giving up of doing it .. */
  gettimeofday (&cur_time, NULL);
  start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
  do
    {
      usleep(timeout_msec*10);
      poll_result = ibv_poll_cq (res.cq, 1, &wc);
      gettimeofday (&cur_time, NULL);
      cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
    }
  while ((poll_result == 0)
	 && ((cur_time_msec - start_time_msec) < timeout_msec));
  if (poll_result < 0)
    {
/* poll CQ failed */
      fprintf (stderr, "poll CQ failed\n");
      rc = 1;
    }
  else if (poll_result == 0)
    {
/* the CQ is empty */
      fprintf (stderr, "completion wasn't found in the CQ after timeout\n");
      rc = 1;
    }
  else
    {
/* CQE found */
      //fprintf (stdout, "completion was found in CQ with status 0x%x\n", wc.status);
/* check the completion status (here we don't care about the completion opcode */
      if (wc.status != IBV_WC_SUCCESS)
	{
	  fprintf (stderr,
		   "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n",
		   wc.status, wc.vendor_err);
	  rc = 1;
	}
    }
  return rc;
}

int post_send (rdma_resources & res,
           DDS_UnsignedLongLong remote_addr,
           DDS_UnsignedLong rkey,
           ibv_wr_opcode opcode,
           void * buf,
           size_t size,
           ibv_mr * mr)
{
  struct ibv_send_wr sr;
  struct ibv_sge sge;
  struct ibv_send_wr *bad_wr = NULL;
  int rc;
  
  memset(&sr, 0, sizeof(ibv_send_wr));
  memset(&sge, 0, sizeof(ibv_sge));
  
  /* prepare the scatter/gather entry */
  sge.addr = (uintptr_t) buf;
  sge.length = size;
  sge.lkey = mr->lkey;
  
  /* prepare the send work request */
  sr.next = NULL;
  sr.wr_id = 0;
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = opcode;
  sr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;

  if (opcode != IBV_WR_SEND)
    {
      sr.wr.rdma.remote_addr = remote_addr;
      sr.wr.rdma.rkey = rkey;
    }
/* there is a Receive Request in the responder side, so we won't get any into RNR flow */
  rc = ibv_post_send (res.qp, &sr, &bad_wr);
  if (rc)
    fprintf (stderr, "failed to post SR\n");
  else
    {
      switch (opcode)
	{
	case IBV_WR_SEND:
	  fprintf (stdout, "Send Request was posted\n");
	  break;
	case IBV_WR_RDMA_READ:
	  //fprintf (stdout, "RDMA Read Request was posted\n");
	  break;
	case IBV_WR_RDMA_WRITE:
	  fprintf (stdout, "RDMA Write Request was posted\n");
	  break;
	default:
	  fprintf (stdout, "Unknown Request was posted\n");
	  break;
	}
    }
  return rc;
}

RDMA_EndPoint::~RDMA_EndPoint()
{ }

