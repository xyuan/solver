#ifndef __UTIL_H
#define __UTIL_H

#include "tensor.h"
#include <map>
#include <vector>
#include <algorithm>
#include <cstdarg>

/*vector IO*/
template <class T>
std::ostream& operator << (std::ostream& os, const std::vector<T>& p) {
	os << p.size() << std::endl;
	os << "{ " << std::endl;
	for(Int i = 0;i < p.size();i++) 
		os << p[i] << std::endl;
	os << "}" << std::endl;
	return os;
}
std::ostream& operator << (std::ostream& os, const std::vector<Int>& p);

template <class T>
std::istream& operator >> (std::istream& is, std::vector<T>& p) {
	Int size;
	char symbol;
	is >> size >> symbol;
	p.resize(size);
	for(Int i = 0;i < size;i++) 
		is >> p[i];
	is >> symbol;
	return is;
}
/*equal Vector*/
template <class T>
bool equal(std::vector<T>& v1,std::vector<T>& v2) {
	Int i,j;
	for(i = 0;i < v1.size();i++) {
		for(j = 0;j < v2.size();j++) {
			if(v1[i] == v2[j]) 
				break;
		}
		if(j == v2.size())
			return false;
	}
	return true;
}

/*Util*/
namespace Util {

	/*others*/
	extern bool Terminated;

	inline Scalar max(Scalar a,Scalar b) {
		return a > b ? a : b;
	}
	inline int compare(std::string& s1,std::string s2) {
		std::string t1 = s1,t2 = s2;
		transform(t1.begin(),t1.end(),t1.begin(),toupper);
		transform(t2.begin(),t2.end(),t2.begin(),toupper);
		return (t1 != t2);
	}
	Int hash_function(std::string s);
	int nextc(std::istream&);
	void cleanup();

	/*option*/
	struct Option {
		Int* val;
		std::vector<std::string> list;
		Option(void* v,Int N, ...) {
			val = (Int*)v;
			std::string str;
			list.assign(N,"");
			va_list ap;
			va_start(ap, N);
			for(Int i = 0;i < N;i++) {
				str = va_arg(ap,char*);
				list[i] = str;
			}
			va_end(ap);
		}
		Int getID(std::string str) {
			for(Int i = 0;i < list.size();i++)
				if(!compare(list[i],str)) return i;
			std::cout << "Unknown parameter : " << str << std::endl;
			return 0;
		}
		friend std::istream& operator >> (std::istream& is, Option& p) {
			std::string str;
			is >> str;
			*(p.val) = p.getID(str);
			return is;
		}
		friend std::ostream& operator << (std::ostream& os, const Option& p) {
			os << p.list[*(p.val)];
			return os;
		}
	};

	/*parameters*/
	template <typename T> 
	class Parameters{
		static std::map<std::string,T*> list;
	public:
		static void enroll(std::string str,T* addr) {
			list[str] = addr;
		}
		static bool read(std::string str,std::istream& is) {
			typename std::map<std::string,T*>::iterator it = list.find(str);
			if(it != list.end()) {
				is >> *(it->second);
				std::cout << *(it->second);
				return true;
			}
			return false;
		}
	};

	template <typename T>
	std::map<std::string,T*> Parameters<T>::list;

	typedef Parameters<Int>         IntParams;
	typedef Parameters<Scalar>      ScalarParams;
	typedef Parameters<Vector>      VectorParams;
	typedef Parameters<STensor>     STensorParams;
	typedef Parameters<Tensor>      TensorParams;
	typedef Parameters<std::string> StringParams;
	typedef Parameters<Option>      OptionParams;
	typedef Parameters< std::vector<Vector> >    VerticesParams;

	extern void read_params(std::istream&);
	extern void read_param(std::istream&,std::string);
	/*end*/
}

#endif
