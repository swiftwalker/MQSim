#ifndef DEFINITIONS_H
#define DEFINITIONS_H

#include<cstdint>
#include<string>
#include<iostream>
#include<sstream>
#include<stdexcept>

typedef uint64_t sim_time_type;
typedef uint16_t stream_id_type;
typedef sim_time_type data_timestamp_type;

#define INVALID_TIME 18446744073709551615ULL
#define T0 0
#define INVALID_TIME_STAMP 18446744073709551615ULL
#define MAXIMUM_TIME 18446744073709551615ULL
#define ONE_SECOND 1000000000
typedef std::string sim_object_id_type;

#define CurrentTimeStamp Simulator->Time()
// Embedding-safety patch: original macro did `std::cin.get(); exit(1);`, which —
// when MQSim is loaded as a Python extension — blocks on stdin and then kills the
// host interpreter. Throw instead so failures propagate to Python as exceptions.
#define PRINT_ERROR(MSG) {\
							std::ostringstream _mqsim_err_ss;\
							_mqsim_err_ss << "ERROR:" << MSG;\
							throw std::runtime_error(_mqsim_err_ss.str());\
						 }
#define PRINT_MESSAGE(M) std::cout << M << std::endl;
#define DEBUG(M) //std::cout<<M<<std::endl;
#define DEBUG2(M) //std::cout<<M<<std::endl;
#define SIM_TIME_TO_MICROSECONDS_COEFF 1000
#define SIM_TIME_TO_SECONDS_COEFF 1000000000
#endif // !DEFINITIONS_H
