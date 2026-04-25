#ifndef PROJECT_X_Obfuscate_H
#define PROJECT_X_Obfuscate_H
#ifndef STRING_OBFUSCATE_DEFAULT_KEY
#define STRING_OBFUSCATE_DEFAULT_KEY 0x9CBA7654331B2B69ull
#endif

namespace ay {
    using size_type = unsigned long long;
    using key_type = unsigned long long;

    constexpr void cipher(char* data, size_type size, key_type key) {
        for (size_type i = 0; i < size; i++) {
            data[i] ^= char(key >> ((i % 8) * 8));
        }
    }
    template <size_type N, key_type KEY>
    class obfuscator {
    public:
        constexpr obfuscator(const char* data) {
            for (size_type i = 0; i < N; i++) {
                m_data[i] = data[i];
            }
            cipher(m_data, N, KEY);
        }
        constexpr const char* data() const {
            return &m_data[0];
        }
        constexpr size_type size() const {
            return N;
        }
        constexpr key_type key() const {
            return KEY;
        }
    private:
        char m_data[N]{};
    };
    template <size_type N, key_type KEY>
    class obfuscated_data {
    public:
        obfuscated_data(const obfuscator<N, KEY>& obfuscator) {
            for (size_type i = 0; i < N; i++) {
                m_data[i] = obfuscator.data()[i];
            }
        }
        ~obfuscated_data() {
            for (size_type i = 0; i < N; i++) {
                m_data[i] = 0;
            }
        }
        operator char*() {
            decrypt();
            return m_data;
        }
        void decrypt() {
            if (m_encrypted) {
                cipher(m_data, N, KEY);
                m_encrypted = false;
            }
        }
        void encrypt() {
            if (!m_encrypted) {
                cipher(m_data, N, KEY);
                m_encrypted = true;
            }
        }
        bool is_encrypted() const {
            return m_encrypted;
        }
    private:
        char m_data[N];
        bool m_encrypted{ true };
    };
    template <size_type N, key_type KEY = STRING_OBFUSCATE_DEFAULT_KEY>
    constexpr auto make_obfuscator(const char(&data)[N]) {
        return obfuscator<N, KEY>(data);
    }
}
#define STRING_OBFUSCATE(data) STRING_OBFUSCATE_KEY(data, STRING_OBFUSCATE_DEFAULT_KEY)
#define OBFUSCATE(data) STRING_OBFUSCATE_KEY(data, STRING_OBFUSCATE_DEFAULT_KEY)
#define STRING_OBFUSCATE_KEY(data, key) \
	[]() -> ay::obfuscated_data<sizeof(data)/sizeof(data[0]), key>& { \
		static_assert(sizeof(decltype(key)) == sizeof(ay::key_type), "key must be a 64 bit unsigned integer"); \
		static_assert((key) >= (1ull << 56), "key must span all 8 bytes"); \
		constexpr auto n = sizeof(data)/sizeof(data[0]); \
		constexpr auto obfuscator = ay::make_obfuscator<n, key>(data); \
		static auto obfuscated_data = ay::obfuscated_data<n, key>(obfuscator); \
		return obfuscated_data; \
	}()

#endif
