/*********************************************************************************************
(c) 2005-2014 Copyright, Real-Time Innovations, Inc.  All rights reserved.
RTI grants Licensee a license to use, modify, compile, and create derivative works
of the Software.  Licensee has the right to distribute object form only for use with RTI
products.  The Software is provided "as is", with no warranty of any type, including
any warranty for fitness for any purpose. RTI is under no obligation to maintain or
support the Software.  RTI shall not be liable for any incidental or consequential
damages arising out of the use or inability to use the software.
**********************************************************************************************/

#ifndef RTI_RDMA_DDS_DLLEXPORT_H
#define RTI_RDMA_DDS_DLLEXPORT_H

#ifdef RDMA_DDS_NO_HEADER_ONLY

#ifdef RTI_WIN32
  #ifdef BUILD_RDMA_DDS_DLL
    #define RDMA_DDS_DLL_EXPORT   __declspec( dllexport ) 
    #define RDMA_DDS_DECLSPEC    __cdecl 
    #define RDMA_DDS_EXPIMP_TEMPLATE
  #else
    #define RDMA_DDS_DLL_EXPORT   __declspec( dllimport ) 
    #define RDMA_DDS_DECLSPEC    __cdecl 
    #define RDMA_DDS_EXPIMP_TEMPLATE extern
  #endif
#else
  #define RDMA_DDS_DLL_EXPORT  
  #define RDMA_DDS_DECLSPEC
  #define RDMA_DDS_EXPIMP_TEMPLATE
#endif

#define RDMA_DDS_INLINE 

#else
  #define RDMA_DDS_DLL_EXPORT 
  #define RDMA_DDS_DECLSPEC
  #define RDMA_DDS_EXPIMP_TEMPLATE 
  #define RDMA_DDS_INLINE inline
#endif // RDMA_DDS_NO_HEADER_ONLY

#endif // RTI_RDMA_DDS_DLLEXPORT_H
