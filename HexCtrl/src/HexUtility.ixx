module;
/****************************************************************************************
* Copyright © 2018-present Jovibor https://github.com/jovibor/                          *
* Hex Control for Windows applications.                                                 *
* Official git repository: https://github.com/jovibor/HexCtrl/                          *
* This software is available under "The HexCtrl License", see the LICENSE file.         *
****************************************************************************************/
#include <SDKDDKVer.h>
#include "../HexCtrl.h"
#include <afxwin.h>
#include <bit>
#include <cassert>
#include <cwctype>
#include <format>
#include <intrin.h>
#include <locale>
#include <optional>
#include <source_location>
#include <string>
#include <unordered_map>
export module HEXCTRL.HexUtility;

export import StrToNum;
export import ListEx;

export namespace HEXCTRL::INTERNAL {
	//Time calculation constants and structs.
	constexpr auto g_uFTTicksPerMS = 10000U;            //Number of 100ns intervals in a milli-second.
	constexpr auto g_uFTTicksPerSec = 10000000U;        //Number of 100ns intervals in a second.
	constexpr auto g_uHoursPerDay = 24U;                //24 hours per day.
	constexpr auto g_uSecondsPerHour = 3600U;           //3600 seconds per hour.
	constexpr auto g_uFileTime1582OffsetDays = 6653U;   //FILETIME is based upon 1 Jan 1601 whilst GUID time is from 15 Oct 1582. Add 6653 days to convert to GUID time.
	constexpr auto g_ulFileTime1970_LOW = 0xd53e8000U;  //1st Jan 1970 as FILETIME.
	constexpr auto g_ulFileTime1970_HIGH = 0x019db1deU; //Used for Unix and Java times.
	constexpr auto g_ullUnixEpochDiff = 11644473600ULL; //Number of ticks from FILETIME epoch of 1st Jan 1601 to Unix epoch of 1st Jan 1970.

	//Get data from IHexCtrl's given offset converted to a necessary type.
	template<typename T>
	[[nodiscard]] T GetIHexTData(const IHexCtrl& refHexCtrl, ULONGLONG ullOffset)
	{
		const auto spnData = refHexCtrl.GetData({ .ullOffset { ullOffset }, .ullSize { sizeof(T) } });
		assert(!spnData.empty());
		return *reinterpret_cast<T*>(spnData.data());
	}

	//Set data of a necessary type to IHexCtrl's given offset.
	template<typename T>
	void SetIHexTData(IHexCtrl& refHexCtrl, ULONGLONG ullOffset, T tData)
	{
		//Data overflow check.
		assert(ullOffset + sizeof(T) <= refHexCtrl.GetDataSize());
		if (ullOffset + sizeof(T) > refHexCtrl.GetDataSize())
			return;

		refHexCtrl.ModifyData({ .eModifyMode { EHexModifyMode::MODIFY_ONCE },
			.spnData { reinterpret_cast<std::byte*>(&tData), sizeof(T) },
			.vecSpan { { ullOffset, sizeof(T) } } });
	}

	template<typename T> concept TSize1248 = (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

	//Bytes swap for types of 2, 4, or 8 byte size.
	template<TSize1248 T> [[nodiscard]] constexpr T ByteSwap(T tData)noexcept
	{
		//Since a swapping-data type can be any type of 2, 4, or 8 bytes size,
		//we first bit_cast swapping-data to an integral type of the same size,
		//then byte-swapping and then bit_cast to the original type back.
		if constexpr (sizeof(T) == sizeof(std::uint8_t)) { //1 byte.
			return tData;
		}
		else if constexpr (sizeof(T) == sizeof(std::uint16_t)) { //2 bytes.
			auto wData = std::bit_cast<std::uint16_t>(tData);
			if (std::is_constant_evaluated()) {
				wData = static_cast<std::uint16_t>((wData << 8) | (wData >> 8));
				return std::bit_cast<T>(wData);
			}
			return std::bit_cast<T>(_byteswap_ushort(wData));
		}
		else if constexpr (sizeof(T) == sizeof(std::uint32_t)) { //4 bytes.
			auto ulData = std::bit_cast<std::uint32_t>(tData);
			if (std::is_constant_evaluated()) {
				ulData = (ulData << 24) | ((ulData << 8) & 0x00FF'0000U)
					| ((ulData >> 8) & 0x0000'FF00U) | (ulData >> 24);
				return std::bit_cast<T>(ulData);
			}
			return std::bit_cast<T>(_byteswap_ulong(ulData));
		}
		else if constexpr (sizeof(T) == sizeof(std::uint64_t)) { //8 bytes.
			auto ullData = std::bit_cast<std::uint64_t>(tData);
			if (std::is_constant_evaluated()) {
				ullData = (ullData << 56) | ((ullData << 40) & 0x00FF'0000'0000'0000ULL)
					| ((ullData << 24) & 0x0000'FF00'0000'0000ULL) | ((ullData << 8) & 0x0000'00FF'0000'0000ULL)
					| ((ullData >> 8) & 0x0000'0000'FF00'0000ULL) | ((ullData >> 24) & 0x0000'0000'00FF'0000ULL)
					| ((ullData >> 40) & 0x0000'0000'0000'FF00ULL) | (ullData >> 56);
				return std::bit_cast<T>(ullData);
			}
			return std::bit_cast<T>(_byteswap_uint64(ullData));
		}
	}

#if defined(_M_IX86) || defined(_M_X64)
	template<typename T> concept TVec128 = (std::is_same_v<T, __m128> || std::is_same_v<T, __m128i> || std::is_same_v<T, __m128d>);
	template<typename T> concept TVec256 = (std::is_same_v<T, __m256> || std::is_same_v<T, __m256i> || std::is_same_v<T, __m256d>);

	template<TSize1248 TIntegral, TVec128 TVec>	//Bytes swap inside vector types: __m128, __m128i, __m128d.
	[[nodiscard]] auto ByteSwapVec(const TVec m128T) -> TVec
	{
		if constexpr (std::is_same_v<TVec, __m128i>) { //Integrals.
			if constexpr (sizeof(TIntegral) == sizeof(std::uint8_t)) { //1 bytes.
				return m128T;
			}
			else if constexpr (sizeof(TIntegral) == sizeof(std::uint16_t)) { //2 bytes.
				const auto m128iMask = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
				return _mm_shuffle_epi8(m128T, m128iMask);
			}
			else if constexpr (sizeof(TIntegral) == sizeof(std::uint32_t)) { //4 bytes.
				const auto m128iMask = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
				return _mm_shuffle_epi8(m128T, m128iMask);
			}
			else if constexpr (sizeof(TIntegral) == sizeof(std::uint64_t)) { //8 bytes.
				const auto m128iMask = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
				return _mm_shuffle_epi8(m128T, m128iMask);
			}
		}
		else if constexpr (std::is_same_v<TVec, __m128>) { //Floats.
			alignas(16) float flData[4];
			_mm_store_ps(flData, m128T); //Loading m128T into local float array.
			const auto m128iData = _mm_load_si128(reinterpret_cast<__m128i*>(flData)); //Loading array as __m128i (convert).
			const auto m128iMask = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
			const auto m128iSwapped = _mm_shuffle_epi8(m128iData, m128iMask); //Swapping bytes.
			_mm_store_si128(reinterpret_cast<__m128i*>(flData), m128iSwapped); //Loading m128iSwapped back into local array.
			return _mm_load_ps(flData); //Returning local array as __m128.
		}
		else if constexpr (std::is_same_v<TVec, __m128d>) { //Doubles.
			alignas(16) double dbllData[2];
			_mm_store_pd(dbllData, m128T); //Loading m128T into local double array.
			const auto m128iData = _mm_load_si128(reinterpret_cast<__m128i*>(dbllData)); //Loading array as __m128i (convert).
			const auto m128iMask = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
			const auto m128iSwapped = _mm_shuffle_epi8(m128iData, m128iMask); //Swapping bytes.
			_mm_store_si128(reinterpret_cast<__m128i*>(dbllData), m128iSwapped); //Loading m128iSwapped back into local array.
			return _mm_load_pd(dbllData); //Returning local array as __m128d.
		}
	}

	template<TSize1248 TIntegral, TVec256 TVec>	//Bytes swap inside vector types: __m256, __m256i, __m256d.
	[[nodiscard]] auto ByteSwapVec(const TVec m256T) -> TVec
	{
		if constexpr (std::is_same_v<TVec, __m256i>) { //Integrals.
			if constexpr (sizeof(TIntegral) == sizeof(std::uint8_t)) { //1 bytes.
				return m256T;
			}
			else if constexpr (sizeof(TIntegral) == sizeof(std::uint16_t)) { //2 bytes.
				const auto m256iMask = _mm256_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14, 17, 16,
					19, 18, 21, 20, 23, 22, 25, 24, 27, 26, 29, 28, 31, 30);
				return _mm256_shuffle_epi8(m256T, m256iMask);
			}
			else if constexpr (sizeof(TIntegral) == sizeof(std::uint32_t)) { //4 bytes.
				const auto m256iMask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
					19, 18, 17, 16, 23, 22, 21, 20, 27, 26, 25, 24, 31, 30, 29, 28);
				return _mm256_shuffle_epi8(m256T, m256iMask);
			}
			else if constexpr (sizeof(TIntegral) == sizeof(std::uint64_t)) { //8 bytes.
				const auto m256iMask = _mm256_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
					23, 22, 21, 20, 19, 18, 17, 16, 31, 30, 29, 28, 27, 26, 25, 24);
				return _mm256_shuffle_epi8(m256T, m256iMask);
			}
		}
		else if constexpr (std::is_same_v<TVec, __m256>) { //Floats.
			alignas(32) float flData[8];
			_mm256_store_ps(flData, m256T); //Loading m256T into local float array.
			const auto m256iData = _mm256_load_si256(reinterpret_cast<__m256i*>(flData)); //Loading array as __m256i (convert).
			const auto m256iMask = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
					19, 18, 17, 16, 23, 22, 21, 20, 27, 26, 25, 24, 31, 30, 29, 28);
			const auto m256iSwapped = _mm256_shuffle_epi8(m256iData, m256iMask); //Swapping bytes.
			_mm256_store_si256(reinterpret_cast<__m256i*>(flData), m256iSwapped); //Loading m256iSwapped back into local array.
			return _mm256_load_ps(flData); //Returning local array as __m256.
		}
		else if constexpr (std::is_same_v<TVec, __m256d>) { //Doubles.
			alignas(32) double dbllData[4];
			_mm256_store_pd(dbllData, m256T); //Loading m256T into local double array.
			const auto m256iData = _mm256_load_si256(reinterpret_cast<__m256i*>(dbllData)); //Loading array as __m256i (convert).
			const auto m256iMask = _mm256_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
					23, 22, 21, 20, 19, 18, 17, 16, 31, 30, 29, 28, 27, 26, 25, 24);
			const auto m256iSwapped = _mm256_shuffle_epi8(m256iData, m256iMask); //Swapping bytes.
			_mm256_store_si256(reinterpret_cast<__m256i*>(dbllData), m256iSwapped); //Loading m256iSwapped back into local array.
			return _mm256_load_pd(dbllData); //Returning local array as __m256d.
		}
	}

	[[nodiscard]] bool HasAVX2() {
		const static bool fHasAVX2 = []() {
			int arrInfo[4] { };
			__cpuid(arrInfo, 0);
			if (arrInfo[0] >= 7) {
				__cpuid(arrInfo, 7);
				return (arrInfo[1] & (1 << 5)) != 0;
			}
			return false;
			}();
		return fHasAVX2;
	}
#endif // ^^^ !defined(_M_IX86) && !defined(_M_X64)

	template<TSize1248 T> [[nodiscard]] constexpr T BitReverse(T tData) {
		T tReversed { };
		constexpr auto iBitsCount = sizeof(T) * 8;
		for (auto i = 0; i < iBitsCount; ++i, tData >>= 1) {
			tReversed = (tReversed << 1) | (tData & 1);
		}
		return tReversed;
	}

	//Converts every two numeric wchars to one respective hex character: "56"->V(0x56), "7A"->z(0x7A), etc...
	//chWc - a wildcard if any.
	[[nodiscard]] auto NumStrToHex(std::wstring_view wsv, char chWc = 0) -> std::optional<std::string>
	{
		const auto fWc = chWc != 0; //Is wildcard used?
		std::wstring wstrFilter = L"0123456789AaBbCcDdEeFf"; //Allowed characters.

		if (fWc) {
			wstrFilter += chWc;
		}

		if (wsv.find_first_not_of(wstrFilter) != std::wstring_view::npos)
			return std::nullopt;

		std::string strHexTmp;
		for (auto iterBegin = wsv.begin(); iterBegin != wsv.end();) {
			if (fWc && static_cast<char>(*iterBegin) == chWc) { //Skip wildcard.
				++iterBegin;
				strHexTmp += chWc;
				continue;
			}

			//Extract two current wchars and pass it to StringToNum as wstring.
			const std::size_t nOffsetCurr = iterBegin - wsv.begin();
			const auto nSize = nOffsetCurr + 2 <= wsv.size() ? 2 : 1;
			if (const auto optNumber = stn::StrToUInt8(wsv.substr(nOffsetCurr, nSize), 16); optNumber) {
				iterBegin += nSize;
				strHexTmp += *optNumber;
			}
			else
				return std::nullopt;
		}

		return { std::move(strHexTmp) };
	}

	[[nodiscard]] auto WstrToStr(std::wstring_view wsv, UINT uCodePage = CP_UTF8) -> std::string
	{
		const auto iSize = WideCharToMultiByte(uCodePage, 0, wsv.data(), static_cast<int>(wsv.size()), nullptr, 0, nullptr, nullptr);
		std::string str(iSize, 0);
		WideCharToMultiByte(uCodePage, 0, wsv.data(), static_cast<int>(wsv.size()), str.data(), iSize, nullptr, nullptr);
		return str;
	}

	[[nodiscard]] auto StrToWstr(std::string_view sv, UINT uCodePage = CP_UTF8) -> std::wstring
	{
		const auto iSize = MultiByteToWideChar(uCodePage, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
		std::wstring wstr(iSize, 0);
		MultiByteToWideChar(uCodePage, 0, sv.data(), static_cast<int>(sv.size()), wstr.data(), iSize);
		return wstr;
	}

	[[nodiscard]] auto StringToSystemTime(std::wstring_view wsv, DWORD dwFormat) -> std::optional<SYSTEMTIME>
	{
		//dwFormat is a locale specific date format https://docs.microsoft.com/en-gb/windows/win32/intl/locale-idate

		if (wsv.empty())
			return std::nullopt;

		//Normalise the input string by replacing non-numeric characters except space with a `/`.
		//This should regardless of the current date/time separator character.
		std::wstring wstrDateTimeCooked;
		for (const auto wch : wsv) {
			wstrDateTimeCooked += (std::iswdigit(wch) || wch == L' ') ? wch : L'/';
		}

		SYSTEMTIME stSysTime { };
		int iParsedArgs { };

		//Parse date component.
		switch (dwFormat) {
		case 0:	//Month-Day-Year.
			iParsedArgs = swscanf_s(wstrDateTimeCooked.data(), L"%2hu/%2hu/%4hu", &stSysTime.wMonth, &stSysTime.wDay, &stSysTime.wYear);
			break;
		case 1: //Day-Month-Year.
			iParsedArgs = swscanf_s(wstrDateTimeCooked.data(), L"%2hu/%2hu/%4hu", &stSysTime.wDay, &stSysTime.wMonth, &stSysTime.wYear);
			break;
		case 2:	//Year-Month-Day.
			iParsedArgs = swscanf_s(wstrDateTimeCooked.data(), L"%4hu/%2hu/%2hu", &stSysTime.wYear, &stSysTime.wMonth, &stSysTime.wDay);
			break;
		default:
			assert(true);
			return std::nullopt;
		}
		if (iParsedArgs != 3)
			return std::nullopt;

		//Find time seperator, if present.
		if (const auto nPos = wstrDateTimeCooked.find(L' '); nPos != std::wstring::npos) {
			wstrDateTimeCooked = wstrDateTimeCooked.substr(nPos + 1);

			//Parse time component HH:MM:SS.mmm.
			if (swscanf_s(wstrDateTimeCooked.data(), L"%2hu/%2hu/%2hu/%3hu", &stSysTime.wHour, &stSysTime.wMinute,
				&stSysTime.wSecond, &stSysTime.wMilliseconds) != 4)
				return std::nullopt;
		}

		return stSysTime;
	}

	[[nodiscard]] auto StringToFileTime(std::wstring_view wsv, DWORD dwFormat) -> std::optional<FILETIME>
	{
		std::optional<FILETIME> optFT { std::nullopt };
		if (auto optSysTime = StringToSystemTime(wsv, dwFormat); optSysTime) {
			if (FILETIME ftTime; SystemTimeToFileTime(&*optSysTime, &ftTime) != FALSE) {
				optFT = ftTime;
			}
		}
		return optFT;
	}

	[[nodiscard]] auto SystemTimeToString(SYSTEMTIME stSysTime, DWORD dwFormat, wchar_t wchSepar) -> std::wstring
	{
		if (dwFormat > 2 || stSysTime.wDay == 0 || stSysTime.wDay > 31 || stSysTime.wMonth == 0 || stSysTime.wMonth > 12
			|| stSysTime.wYear > 9999 || stSysTime.wHour > 23 || stSysTime.wMinute > 59 || stSysTime.wSecond > 59
			|| stSysTime.wMilliseconds > 999)
			return L"N/A";

		std::wstring_view wsvFmt;
		switch (dwFormat) {
		case 0:	//0:Month/Day/Year HH:MM:SS.mmm.
			wsvFmt = L"{1:02d}{7}{0:02d}{7}{2} {3:02d}:{4:02d}:{5:02d}.{6:03d}";
			break;
		case 1: //1:Day/Month/Year HH:MM:SS.mmm.
			wsvFmt = L"{0:02d}{7}{1:02d}{7}{2} {3:02d}:{4:02d}:{5:02d}.{6:03d}";
			break;
		case 2: //2:Year/Month/Day HH:MM:SS.mmm.
			wsvFmt = L"{2}{7}{1:02d}{7}{0:02d} {3:02d}:{4:02d}:{5:02d}.{6:03d}";
			break;
		default:
			break;
		}

		return std::vformat(wsvFmt, std::make_wformat_args(stSysTime.wDay, stSysTime.wMonth, stSysTime.wYear,
			stSysTime.wHour, stSysTime.wMinute, stSysTime.wSecond, stSysTime.wMilliseconds, wchSepar));
	}

	[[nodiscard]] auto FileTimeToString(FILETIME stFileTime, DWORD dwFormat, wchar_t wchSepar) -> std::wstring
	{
		if (SYSTEMTIME stSysTime; FileTimeToSystemTime(&stFileTime, &stSysTime) != FALSE) {
			return SystemTimeToString(stSysTime, dwFormat, wchSepar);
		}

		return L"N/A";
	}

	//String of a date/time in the given format with the given date separator.
	[[nodiscard]] auto GetDateFormatString(DWORD dwFormat, wchar_t wchSepar) -> std::wstring
	{
		std::wstring_view wsvFmt;
		switch (dwFormat) {
		case 0:	//Month-Day-Year.
			wsvFmt = L"MM{0}DD{0}YYYY HH:MM:SS.mmm";
			break;
		case 1: //Day-Month-Year.
			wsvFmt = L"DD{0}MM{0}YYYY HH:MM:SS.mmm";
			break;
		case 2:	//Year-Month-Day.
			wsvFmt = L"YYYY{0}MM{0}DD HH:MM:SS.mmm";
			break;
		default:
			assert(true);
			break;
		}

		return std::vformat(wsvFmt, std::make_wformat_args(wchSepar));
	}

	template<typename T>
	[[nodiscard]] auto RangeToVecBytes(const T& refData) -> std::vector<std::byte>
	{
		const std::byte* pBegin;
		const std::byte* pEnd;
		if constexpr (std::is_same_v<T, std::string>) {
			pBegin = reinterpret_cast<const std::byte*>(refData.data());
			pEnd = pBegin + refData.size();
		}
		else if constexpr (std::is_same_v<T, std::wstring>) {
			pBegin = reinterpret_cast<const std::byte*>(refData.data());
			pEnd = pBegin + refData.size() * sizeof(wchar_t);
		}
		else if constexpr (std::is_same_v<T, CStringW>) {
			pBegin = reinterpret_cast<const std::byte*>(refData.GetString());
			pEnd = pBegin + refData.GetLength() * sizeof(wchar_t);
		}
		else {
			pBegin = reinterpret_cast<const std::byte*>(&refData);
			pEnd = pBegin + sizeof(refData);
		}

		return { pBegin, pEnd };
	}

	[[nodiscard]] auto GetLocale() -> std::locale {
		static const auto loc { std::locale("en_US.UTF-8") };
		return loc;
	}

	namespace wnd { //Windows GUI related stuff.
		[[nodiscard]] auto GetHinstance() -> HINSTANCE {
			return AfxGetInstanceHandle();
		}

		auto DefMsgProc(const MSG& stMsg) -> LRESULT {
			return ::DefWindowProcW(stMsg.hwnd, stMsg.message, stMsg.wParam, stMsg.lParam);
		}

		template<typename T>
		auto CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) -> LRESULT
		{
			static std::unordered_map<HWND, T*> uMap;

			//CREATESTRUCTW::lpCreateParams always possesses a `this` pointer, passed to the CreateWindowExW function as lpParam.
			//We save it to the static uMap to have access to this->ProcessMsg() method.
			if (uMsg == WM_CREATE) {
				const auto lpCS = reinterpret_cast<LPCREATESTRUCTW>(lParam);
				uMap[hWnd] = reinterpret_cast<T*>(lpCS->lpCreateParams);
				return 0;
			}

			if (const auto it = uMap.find(hWnd); it != uMap.end()) {
				const auto ret = it->second->ProcessMsg({ .hwnd { hWnd }, .message { uMsg }, .wParam { wParam }, .lParam { lParam } });
				if (uMsg == WM_NCDESTROY) { //Remove hWnd from the map on window destruction.
					uMap.erase(it);
				}
				return ret;
			}

			return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
		}

		template<typename T>
		auto CALLBACK DlgWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) -> INT_PTR {
			//DlgWndProc should return zero for all non-processed messages.
			//In that case messages will be processed by Windows default dialog proc.
			//Non-processed messages should not be passed to DefWindowProcW or DefDlgProcW.
			//Processed messages should return any non-zero value, depending on message type.

			static T* m_pThis { };

			//DialogBoxParamW and CreateDialogParamW dwInitParam arg is sent with WM_INITDIALOG as lParam.
			if (uMsg == WM_INITDIALOG) {
				m_pThis = reinterpret_cast<T*>(lParam);
			}

			if (m_pThis != nullptr) {
				const auto ret = m_pThis->ProcessMsg({ .hwnd { hWnd }, .message { uMsg }, .wParam { wParam }, .lParam { lParam } });
				if (uMsg == WM_NCDESTROY) {
					m_pThis = nullptr;
				}
				return ret;
			}

			return 0;
		}

		void FillSolidRect(HDC hDC, LPCRECT pRC, COLORREF clr) {
			//Replicates CDC::FillSolidRect.
			::SetBkColor(hDC, clr);
			::ExtTextOutW(hDC, 0, 0, ETO_OPAQUE, pRC, nullptr, 0, nullptr);
		}

		class CRect : public RECT {
		public:
			CRect() { left = 0; top = 0; right = 0; bottom = 0; }
			CRect(const RECT& rc) { ::CopyRect(this, &rc); }
			CRect(LPCRECT pRC) { ::CopyRect(this, pRC); }
			[[nodiscard]] int Width()const { return right - left; }
			[[nodiscard]] int Height()const { return bottom - top; }
			[[nodiscard]] bool PtInRect(POINT pt)const { return ::PtInRect(this, pt); }
			[[nodiscard]] bool IsRectEmpty()const { return ::IsRectEmpty(this); }
			[[nodiscard]] bool IsRectNull()const { return (left == 0 && right == 0 && top == 0 && bottom == 0); }
			void DeflateRect(int x, int y) { ::InflateRect(this, -x, -y); }
			void DeflateRect(SIZE size) { ::InflateRect(this, -size.cx, -size.cy); }
			void DeflateRect(LPCRECT pRC) { left += pRC->left; top += pRC->top; right -= pRC->right; bottom -= pRC->bottom; }
			void DeflateRect(int l, int t, int r, int b) { left += l; top += t; right -= r; bottom -= b; }
			void OffsetRect(int x, int y) { ::OffsetRect(this, x, y); }
			void OffsetRect(POINT pt) { ::OffsetRect(this, pt.x, pt.y); }
			void SetRect(int x1, int y1, int x2, int y2) { ::SetRect(this, x1, y1, x2, y2); }
			operator LPRECT() { return this; }
			operator LPCRECT()const { return this; }
			CRect& operator=(const RECT& rc) { ::CopyRect(this, &rc); return *this; }
		};

		class CWnd {
		public:
			CWnd() = default;
			CWnd(HWND hWnd) { Attach(hWnd); }
			~CWnd() { Detach(); }
			CWnd operator=(const CWnd&) = delete;
			CWnd operator=(HWND) = delete;
			operator HWND()const { return m_hWnd; }
			[[nodiscard]] bool operator==(const CWnd& rhs)const { return m_hWnd == rhs.m_hWnd; }
			[[nodiscard]] bool operator==(const HWND hWnd)const { return m_hWnd == hWnd; }
			void Attach(HWND hWnd) { assert(::IsWindow(hWnd)); m_hWnd = hWnd; }
			[[nodiscard]] auto ChildWindowFromPoint(POINT pt)const->HWND {
				assert(IsWindow()); return ::ChildWindowFromPoint(m_hWnd, pt);
			}
			void ClientToScreen(LPRECT pRC)const {
				assert(IsWindow()); ::ClientToScreen(m_hWnd, reinterpret_cast<LPPOINT>(pRC));
				::ClientToScreen(m_hWnd, (reinterpret_cast<LPPOINT>(pRC)) + 1);
			}
			bool DestroyWindow()const { assert(IsWindow()); return ::DestroyWindow(m_hWnd); }
			void Detach() { m_hWnd = nullptr; }
			[[nodiscard]] auto GetClientRect()const->RECT {
				assert(IsWindow()); RECT rc; ::GetClientRect(m_hWnd, &rc); return rc;
			}
			void EndDialog(INT_PTR iResult)const { assert(IsWindow()); ::EndDialog(m_hWnd, iResult); }
			[[nodiscard]] auto GetDC()const->HDC { assert(IsWindow()); return ::GetDC(m_hWnd); }
			[[nodiscard]] auto GetDlgItem(int iIDCtrl)const->HWND { assert(IsWindow()); return ::GetDlgItem(m_hWnd, iIDCtrl); }
			[[nodiscard]] auto GetHFont()const->HFONT {
				assert(IsWindow()); return reinterpret_cast<HFONT>(::SendMessageW(m_hWnd, WM_GETFONT, 0, 0));
			}
			[[nodiscard]] auto GetHWND()const->HWND { return m_hWnd; }
			[[nodiscard]] auto GetLogFont()const->std::optional<LOGFONTW> {
				if (const auto hFont = GetHFont(); hFont != nullptr) {
					LOGFONTW lf { }; ::GetObjectW(hFont, sizeof(lf), &lf); return lf;
				}
				return std::nullopt;
			}
			[[nodiscard]] auto GetParent()const->HWND { assert(IsWindow()); return ::GetParent(m_hWnd); }
			[[nodiscard]] auto GetWindowDC()const->HDC { assert(IsWindow()); return ::GetWindowDC(m_hWnd); }
			[[nodiscard]] auto GetWndRect()const->RECT {
				assert(IsWindow()); RECT rc; ::GetWindowRect(m_hWnd, &rc); return rc;
			}
			[[nodiscard]] auto GetWndText()const->std::wstring {
				assert(IsWindow());	wchar_t buff[MAX_PATH]; ::GetWindowTextW(m_hWnd, buff, std::size(buff)); return buff;
			}
			[[nodiscard]] auto GetWndTextSize()const->DWORD { assert(IsWindow()); return ::GetWindowTextLengthW(m_hWnd); }
			void Invalidate(bool fErase)const { assert(IsWindow()); ::InvalidateRect(m_hWnd, nullptr, fErase); }
			[[nodiscard]] bool IsDlgMessage(MSG* pMsg)const { return ::IsDialogMessageW(m_hWnd, pMsg); }
			[[nodiscard]] bool IsNull()const { return m_hWnd == nullptr; }
			[[nodiscard]] bool IsWindow()const { return::IsWindow(m_hWnd); }
			[[nodiscard]] bool IsWndTextEmpty()const { return GetWndTextSize() == 0; }
			void KillTimer(UINT_PTR uID)const {
				assert(IsWindow()); ::KillTimer(m_hWnd, uID);
			}
			int MapWindowPoints(HWND hWndTo, LPRECT pRC)const {
				assert(IsWindow()); return ::MapWindowPoints(m_hWnd, hWndTo, reinterpret_cast<LPPOINT>(pRC), 2);
			}
			bool RedrawWindow(LPCRECT pRC = nullptr, HRGN hrgn = nullptr,
				UINT uFlags = RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE)const {
				assert(IsWindow()); return static_cast<bool>(::RedrawWindow(m_hWnd, pRC, hrgn, uFlags));
			}
			int ReleaseDC(HDC hDC)const { assert(IsWindow()); return ::ReleaseDC(m_hWnd, hDC); }
			auto SetTimer(UINT_PTR uID, UINT uElapse, TIMERPROC pFN = nullptr)const->UINT_PTR {
				assert(IsWindow()); return ::SetTimer(m_hWnd, uID, uElapse, pFN);
			}
			void ScreenToClient(LPPOINT pPT)const { assert(IsWindow()); ::ScreenToClient(m_hWnd, pPT); }
			void ScreenToClient(POINT& pt)const { ScreenToClient(&pt); }
			void ScreenToClient(LPRECT pRC)const {
				ScreenToClient(reinterpret_cast<LPPOINT>(pRC)); ScreenToClient(reinterpret_cast<LPPOINT>(pRC) + 1);
			}
			auto SendMsg(UINT uMsg, WPARAM wParam = 0, LPARAM lParam = 0)const {
				assert(IsWindow()); ::SendMessageW(m_hWnd, uMsg, wParam, lParam);
			}
			auto SetCapture()const->HWND { assert(IsWindow()); return ::SetCapture(m_hWnd); }
			auto SetWndClassLong(int iIndex, LONG_PTR dwNewLong)const->ULONG_PTR {
				assert(IsWindow()); return ::SetClassLongPtrW(m_hWnd, iIndex, dwNewLong);
			}
			void SetFocus()const { assert(IsWindow()); ::SetFocus(m_hWnd); }
			void SetWindowPos(HWND hWndAfter, int iX, int iY, int iWidth, int iHeight, UINT uFlags)const {
				assert(IsWindow()); ::SetWindowPos(m_hWnd, hWndAfter, iX, iY, iWidth, iHeight, uFlags);
			}
			void SetWndText(LPCWSTR pwsz)const { assert(IsWindow()); ::SetWindowTextW(m_hWnd, pwsz); }
			void SetWndText(const std::wstring& wstr)const { SetWndText(wstr.data()); }
			void SetRedraw(bool fRedraw)const { assert(IsWindow()); ::SendMessageW(m_hWnd, WM_SETREDRAW, fRedraw, 0); }
			bool ShowWindow(int iCmdShow)const { assert(IsWindow()); return ::ShowWindow(m_hWnd, iCmdShow); }
		protected:
			HWND m_hWnd { }; //Windows window handle.
		};

		class CWndCombo : public CWnd {
		public:
			int AddString(LPCWSTR pwsz)const {
				assert(IsWindow());
				return static_cast<int>(::SendMessageW(m_hWnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pwsz)));
			}
			int DeleteString(int iIndex)const {
				assert(IsWindow()); return static_cast<int>(::SendMessageW(m_hWnd, CB_DELETESTRING, iIndex, 0));
			}
			[[nodiscard]] int GetCount()const {
				assert(IsWindow()); return static_cast<int>(::SendMessageW(m_hWnd, CB_GETCOUNT, 0, 0));
			}
			[[nodiscard]] int GetCurSel()const {
				assert(IsWindow()); return static_cast<int>(::SendMessageW(m_hWnd, CB_GETCURSEL, 0, 0));
			}
			[[nodiscard]] auto GetItemData(int iIndex)const->DWORD_PTR {
				assert(IsWindow()); return ::SendMessageW(m_hWnd, CB_GETITEMDATA, iIndex, 0);
			}
			void SetCurSel(int iIndex)const { assert(IsWindow()); ::SendMessageW(m_hWnd, CB_SETCURSEL, iIndex, 0); }
			void SetItemData(int iIndex, DWORD_PTR dwData)const {
				assert(IsWindow()); ::SendMessageW(m_hWnd, CB_SETITEMDATA, iIndex, static_cast<LPARAM>(dwData));
			}
		};

		class CWndProgBar : public CWnd {
		public:
			int SetPos(int iPos)const {
				assert(IsWindow()); return static_cast<int>(::SendMessageW(m_hWnd, PBM_SETPOS, iPos, 0UL));
			}
			void SetRange(int iLower, int iUpper)const {
				assert(IsWindow());
				::SendMessageW(m_hWnd, PBM_SETRANGE32, static_cast<WPARAM>(iLower), static_cast<LPARAM>(iUpper));
			}
		};
	};

#if defined(DEBUG) || defined(_DEBUG)
	void DBG_REPORT(const wchar_t* pMsg, const std::source_location& loc = std::source_location::current()) {
		_wassert(pMsg, StrToWstr(loc.file_name()).data(), loc.line());
	}
#else
	void DBG_REPORT([[maybe_unused]] const wchar_t*) { }
#endif
}