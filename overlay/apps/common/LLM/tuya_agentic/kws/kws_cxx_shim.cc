/* ============================================================================
 * kws_cxx_shim.cc — audio_subsys.a 缺失的 libc++ 符号垫片
 *
 * audio_subsys.a(wekws/TFLite) 是 C++ 库，依赖 libc++ 的约 37 个符号；杰理
 * 工具链没有 libc++（pi32v2-lib 只有 libc/libm/libcompiler_rt），SDK 自带的
 * apps/common/c++/cxx_runtime.cpp 只覆盖 operator new/delete 与 __cxa_*。
 *
 * 本文件按 bitcode 逆向出的确切符号表（llvm-nm --undefined-only 去重）补齐：
 *  - 流/locale 系列（basic_ios/istream/ostream/streambuf/locale/ios_base）：
 *      只在"从文件读模型"的分支才会执行（我们用 NULL 路径走内嵌模型），
 *      一律【打印 + 停机】——串口出现 kws_cxx_missing 说明踩到了未预期路径，
 *      好过静默出错。
 *  - basic_string 方法（append/assign/insert/push_back/resize）：
 *      create(NULL) 的 SSO 空串路径不经过它们；同样先停机兜底。
 *  - __throw_length_error / to_string(int)：同上。
 *  - ctype<char>::id / codecvt<char,...>::id：locale::id 静态数据，给 4 字节
 *      零初始化存储即可（不会被执行路径用到）。
 * 若后续需要真正从文件读模型，再按 libc++ 经典内存布局实现真实版本。
 * ==========================================================================*/
#include <stdint.h>
#include <stdio.h>

extern "C" {

__attribute__((noreturn))
static void kws_cxx_missing(const char *who)
{
    printf("[KWS-CXX] unexpected libc++ call: %s\n", who);
    while (1) { ; }
}

/* ---- locale::id 静态数据（4 字节零存储, 不参与执行） ---- */
uint32_t _ZNSt3__15ctypeIcE2idE;
uint32_t _ZNSt3__17codecvtIcc10_mbstate_tE2idE;

/* ---- __throw_length_error / to_string ---- */
void _ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv(void) { kws_cxx_missing("vector::__throw_length_error"); }
void _ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv(void) { kws_cxx_missing("string::__throw_length_error"); }
void _ZNSt3__19to_stringEi(void) { kws_cxx_missing("to_string(int)"); }

/* ---- locale / ios_base ---- */
void _ZNKSt3__16locale9has_facetERNS0_2idE(void) { kws_cxx_missing("locale::has_facet"); }
void _ZNKSt3__16locale9use_facetERNS0_2idE(void) { kws_cxx_missing("locale::use_facet"); }
void _ZNSt3__16localeC1ERKS0_(void) { kws_cxx_missing("locale::locale(loc)"); }
void _ZNSt3__16localeD1Ev(void) { kws_cxx_missing("locale::~locale"); }
void _ZNSt3__18ios_base4initEPv(void) { kws_cxx_missing("ios_base::init"); }
void _ZNSt3__18ios_base5clearEj(void) { kws_cxx_missing("ios_base::clear"); }
void _ZNKSt3__18ios_base6getlocEv(void) { kws_cxx_missing("ios_base::getloc"); }

/* ---- basic_ios / istream / ostream / streambuf ---- */
void _ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev(void) { kws_cxx_missing("basic_ios::~basic_ios"); }
void _ZNSt3__113basic_istreamIcNS_11char_traitsIcEEED0Ev(void) { kws_cxx_missing("basic_istream::~D0"); }
void _ZNSt3__113basic_istreamIcNS_11char_traitsIcEEED1Ev(void) { kws_cxx_missing("basic_istream::~D1"); }
void _ZNSt3__113basic_istreamIcNS_11char_traitsIcEEED2Ev(void) { kws_cxx_missing("basic_istream::~D2"); }
void _ZTv0_n12_NSt3__113basic_istreamIcNS_11char_traitsIcEEED0Ev(void) { kws_cxx_missing("thunk istream::~D0"); }
void _ZTv0_n12_NSt3__113basic_istreamIcNS_11char_traitsIcEEED1Ev(void) { kws_cxx_missing("thunk istream::~D1"); }
void _ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev(void) { kws_cxx_missing("basic_ostream::~D0"); }
void _ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev(void) { kws_cxx_missing("basic_ostream::~D1"); }
void _ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEED2Ev(void) { kws_cxx_missing("basic_ostream::~D2"); }
void _ZTv0_n12_NSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev(void) { kws_cxx_missing("thunk ostream::~D0"); }
void _ZTv0_n12_NSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev(void) { kws_cxx_missing("thunk ostream::~D1"); }
void _ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE6sentryC1ERS3_(void) { kws_cxx_missing("ostream::sentry::sentry"); }
void _ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE6sentryD1Ev(void) { kws_cxx_missing("ostream::sentry::~sentry"); }
void _ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEElsEi(void) { kws_cxx_missing("ostream::operator<<(int)"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev(void) { kws_cxx_missing("streambuf::streambuf"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev(void) { kws_cxx_missing("streambuf::~streambuf"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE4syncEv(void) { kws_cxx_missing("streambuf::sync"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE5imbueERKNS_6localeE(void) { kws_cxx_missing("streambuf::imbue"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE5uflowEv(void) { kws_cxx_missing("streambuf::uflow"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE6setbufEPcl(void) { kws_cxx_missing("streambuf::setbuf"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE6xsgetnEPcl(void) { kws_cxx_missing("streambuf::xsgetn"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE6xsputnEPKcl(void) { kws_cxx_missing("streambuf::xsputn"); }
void _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE9showmanycEv(void) { kws_cxx_missing("streambuf::showmanyc"); }

/* ---- basic_string 方法（空串 SSO 路径不经过; 停机兜底） ---- */
void _ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc(void) { kws_cxx_missing("string::append(cstr)"); }
void _ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm(void) { kws_cxx_missing("string::append(cstr,n)"); }
void _ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc(void) { kws_cxx_missing("string::assign"); }
void _ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc(void) { kws_cxx_missing("string::insert"); }
void _ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc(void) { kws_cxx_missing("string::push_back"); }
void _ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc(void) { kws_cxx_missing("string::resize"); }

} /* extern "C" */
