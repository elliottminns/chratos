#include <chratos/lib/numbers.hpp>

#include <ed25519-donna/ed25519.h>

#include <blake2/blake2.h>

#include <cryptopp/aes.h>
#include <cryptopp/modes.h>

thread_local CryptoPP::AutoSeededRandomPool chratos::random_pool;

namespace
{
char const * base58_reverse ("~012345678~~~~~~~9:;<=>?@~ABCDE~FGHIJKLMNOP~~~~~~QRSTUVWXYZ[~\\]^_`abcdefghi");
uint8_t base58_decode (char value)
{
	assert (value >= '0');
	assert (value <= '~');
	auto result (base58_reverse[value - 0x30] - 0x30);
	return result;
}
char const * account_lookup ("13456789abcdefghijkmnopqrstuwxyz");
char const * account_reverse ("~0~1234567~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~89:;<=>?@AB~CDEFGHIJK~LMNO~~~~~");
char account_encode (uint8_t value)
{
	assert (value < 32);
	auto result (account_lookup[value]);
	return result;
}
uint8_t account_decode (char value)
{
	assert (value >= '0');
	assert (value <= '~');
	auto result (account_reverse[value - 0x30] - 0x30);
	return result;
}
}

void chratos::uint256_union::encode_account (std::string & destination_a) const
{
	assert (destination_a.empty ());
	destination_a.reserve (64);
	uint64_t check (0);
	blake2b_state hash;
	blake2b_init (&hash, 5);
	blake2b_update (&hash, bytes.data (), bytes.size ());
	blake2b_final (&hash, reinterpret_cast<uint8_t *> (&check), 5);
	chratos::uint512_t number_l (number ());
	number_l <<= 40;
	number_l |= chratos::uint512_t (check);
	for (auto i (0); i < 60; ++i)
	{
		uint8_t r (number_l & static_cast<uint8_t> (0x1f));
		number_l >>= 5;
		destination_a.push_back (account_encode (r));
	}
	destination_a.append ("_rhc"); // chr_
	std::reverse (destination_a.begin (), destination_a.end ());
}

std::string chratos::uint256_union::to_account () const
{
	std::string result;
	encode_account (result);
	return result;
}

bool chratos::uint256_union::decode_account (std::string const & source_a)
{
	auto error (source_a.size () < 5);
	if (!error)
	{
		auto chr_prefix (source_a[0] == 'c' && source_a[1] == 'h' && source_a[2] == 'r' && (source_a[3] == '_' || source_a[3] == '-'));
		auto nano_prefix (source_a[0] == 'n' && source_a[1] == 'a' && source_a[2] == 'n' && source_a[3] == 'o' && (source_a[4] == '_' || source_a[4] == '-'));
		error = (chr_prefix && source_a.size () != 64) || (nano_prefix && source_a.size () != 65);
		if (!error)
		{
			if (chr_prefix || nano_prefix)
			{
				auto i (source_a.begin () + (chr_prefix ? 4 : 5));
				if (*i == '1' || *i == '3')
				{
					chratos::uint512_t number_l;
					for (auto j (source_a.end ()); !error && i != j; ++i)
					{
						uint8_t character (*i);
						error = character < 0x30 || character >= 0x80;
						if (!error)
						{
							uint8_t byte (account_decode (character));
							error = byte == '~';
							if (!error)
							{
								number_l <<= 5;
								number_l += byte;
							}
						}
					}
					if (!error)
					{
						*this = (number_l >> 40).convert_to<chratos::uint256_t> ();
						uint64_t check (number_l & static_cast<uint64_t> (0xffffffffff));
						uint64_t validation (0);
						blake2b_state hash;
						blake2b_init (&hash, 5);
						blake2b_update (&hash, bytes.data (), bytes.size ());
						blake2b_final (&hash, reinterpret_cast<uint8_t *> (&validation), 5);
						error = check != validation;
					}
				}
				else
				{
					error = true;
				}
			}
			else
			{
				error = true;
			}
		}
	}
	return error;
}

chratos::uint256_union::uint256_union (chratos::uint256_t const & number_a)
{
	chratos::uint256_t number_l (number_a);
	for (auto i (bytes.rbegin ()), n (bytes.rend ()); i != n; ++i)
	{
		*i = static_cast<uint8_t> (number_l & static_cast<uint8_t> (0xff));
		number_l >>= 8;
	}
}

bool chratos::uint256_union::operator== (chratos::uint256_union const & other_a) const
{
	return bytes == other_a.bytes;
}

// Construct a uint256_union = AES_ENC_CTR (cleartext, key, iv)
void chratos::uint256_union::encrypt (chratos::raw_key const & cleartext, chratos::raw_key const & key, uint128_union const & iv)
{
	CryptoPP::AES::Encryption alg (key.data.bytes.data (), sizeof (key.data.bytes));
	CryptoPP::CTR_Mode_ExternalCipher::Encryption enc (alg, iv.bytes.data ());
	enc.ProcessData (bytes.data (), cleartext.data.bytes.data (), sizeof (cleartext.data.bytes));
}

bool chratos::uint256_union::is_zero () const
{
	return qwords[0] == 0 && qwords[1] == 0 && qwords[2] == 0 && qwords[3] == 0;
}

std::string chratos::uint256_union::to_string () const
{
	std::string result;
	encode_hex (result);
	return result;
}

bool chratos::uint256_union::operator< (chratos::uint256_union const & other_a) const
{
	return number () < other_a.number ();
}

chratos::uint256_union & chratos::uint256_union::operator^= (chratos::uint256_union const & other_a)
{
	auto j (other_a.qwords.begin ());
	for (auto i (qwords.begin ()), n (qwords.end ()); i != n; ++i, ++j)
	{
		*i ^= *j;
	}
	return *this;
}

chratos::uint256_union chratos::uint256_union::operator^ (chratos::uint256_union const & other_a) const
{
	chratos::uint256_union result;
	auto k (result.qwords.begin ());
	for (auto i (qwords.begin ()), j (other_a.qwords.begin ()), n (qwords.end ()); i != n; ++i, ++j, ++k)
	{
		*k = *i ^ *j;
	}
	return result;
}

chratos::uint256_union::uint256_union (std::string const & hex_a)
{
	decode_hex (hex_a);
}

void chratos::uint256_union::clear ()
{
	qwords.fill (0);
}

chratos::uint256_t chratos::uint256_union::number () const
{
	chratos::uint256_t result;
	auto shift (0);
	for (auto i (bytes.begin ()), n (bytes.end ()); i != n; ++i)
	{
		result <<= shift;
		result |= *i;
		shift = 8;
	}
	return result;
}

void chratos::uint256_union::encode_hex (std::string & text) const
{
	assert (text.empty ());
	std::stringstream stream;
	stream << std::hex << std::noshowbase << std::setw (64) << std::setfill ('0');
	stream << number ();
	text = stream.str ();
}

bool chratos::uint256_union::decode_hex (std::string const & text)
{
	auto error (false);
	if (!text.empty () && text.size () <= 64)
	{
		std::stringstream stream (text);
		stream << std::hex << std::noshowbase;
		chratos::uint256_t number_l;
		try
		{
			stream >> number_l;
			*this = number_l;
			if (!stream.eof ())
			{
				error = true;
			}
		}
		catch (std::runtime_error &)
		{
			error = true;
		}
	}
	else
	{
		error = true;
	}
	return error;
}

void chratos::uint256_union::encode_dec (std::string & text) const
{
	assert (text.empty ());
	std::stringstream stream;
	stream << std::dec << std::noshowbase;
	stream << number ();
	text = stream.str ();
}

bool chratos::uint256_union::decode_dec (std::string const & text)
{
	auto error (text.size () > 78 || (text.size () > 1 && text[0] == '0') || (text.size () > 0 && text[0] == '-'));
	if (!error)
	{
		std::stringstream stream (text);
		stream << std::dec << std::noshowbase;
		chratos::uint256_t number_l;
		try
		{
			stream >> number_l;
			*this = number_l;
			if (!stream.eof ())
			{
				error = true;
			}
		}
		catch (std::runtime_error &)
		{
			error = true;
		}
	}
	return error;
}

chratos::uint256_union::uint256_union (uint64_t value0)
{
	*this = chratos::uint256_t (value0);
}

bool chratos::uint256_union::operator!= (chratos::uint256_union const & other_a) const
{
	return !(*this == other_a);
}

bool chratos::uint512_union::operator== (chratos::uint512_union const & other_a) const
{
	return bytes == other_a.bytes;
}

chratos::uint512_union::uint512_union (chratos::uint512_t const & number_a)
{
	chratos::uint512_t number_l (number_a);
	for (auto i (bytes.rbegin ()), n (bytes.rend ()); i != n; ++i)
	{
		*i = static_cast<uint8_t> (number_l & static_cast<uint8_t> (0xff));
		number_l >>= 8;
	}
}

void chratos::uint512_union::clear ()
{
	bytes.fill (0);
}

chratos::uint512_t chratos::uint512_union::number () const
{
	chratos::uint512_t result;
	auto shift (0);
	for (auto i (bytes.begin ()), n (bytes.end ()); i != n; ++i)
	{
		result <<= shift;
		result |= *i;
		shift = 8;
	}
	return result;
}

void chratos::uint512_union::encode_hex (std::string & text) const
{
	assert (text.empty ());
	std::stringstream stream;
	stream << std::hex << std::noshowbase << std::setw (128) << std::setfill ('0');
	stream << number ();
	text = stream.str ();
}

bool chratos::uint512_union::decode_hex (std::string const & text)
{
	auto error (text.size () > 128);
	if (!error)
	{
		std::stringstream stream (text);
		stream << std::hex << std::noshowbase;
		chratos::uint512_t number_l;
		try
		{
			stream >> number_l;
			*this = number_l;
			if (!stream.eof ())
			{
				error = true;
			}
		}
		catch (std::runtime_error &)
		{
			error = true;
		}
	}
	return error;
}

bool chratos::uint512_union::operator!= (chratos::uint512_union const & other_a) const
{
	return !(*this == other_a);
}

chratos::uint512_union & chratos::uint512_union::operator^= (chratos::uint512_union const & other_a)
{
	uint256s[0] ^= other_a.uint256s[0];
	uint256s[1] ^= other_a.uint256s[1];
	return *this;
}

std::string chratos::uint512_union::to_string () const
{
	std::string result;
	encode_hex (result);
	return result;
}

chratos::raw_key::~raw_key ()
{
	data.clear ();
}

bool chratos::raw_key::operator== (chratos::raw_key const & other_a) const
{
	return data == other_a.data;
}

bool chratos::raw_key::operator!= (chratos::raw_key const & other_a) const
{
	return !(*this == other_a);
}

// This this = AES_DEC_CTR (ciphertext, key, iv)
void chratos::raw_key::decrypt (chratos::uint256_union const & ciphertext, chratos::raw_key const & key_a, uint128_union const & iv)
{
	CryptoPP::AES::Encryption alg (key_a.data.bytes.data (), sizeof (key_a.data.bytes));
	CryptoPP::CTR_Mode_ExternalCipher::Decryption dec (alg, iv.bytes.data ());
	dec.ProcessData (data.bytes.data (), ciphertext.bytes.data (), sizeof (ciphertext.bytes));
}

chratos::uint512_union chratos::sign_message (chratos::raw_key const & private_key, chratos::public_key const & public_key, chratos::uint256_union const & message)
{
	chratos::uint512_union result;
	ed25519_sign (message.bytes.data (), sizeof (message.bytes), private_key.data.bytes.data (), public_key.bytes.data (), result.bytes.data ());
	return result;
}

void chratos::deterministic_key (chratos::uint256_union const & seed_a, uint32_t index_a, chratos::uint256_union & prv_a)
{
	blake2b_state hash;
	blake2b_init (&hash, prv_a.bytes.size ());
	blake2b_update (&hash, seed_a.bytes.data (), seed_a.bytes.size ());
	chratos::uint256_union index (index_a);
	blake2b_update (&hash, reinterpret_cast<uint8_t *> (&index.dwords[7]), sizeof (uint32_t));
	blake2b_final (&hash, prv_a.bytes.data (), prv_a.bytes.size ());
}

chratos::public_key chratos::pub_key (chratos::private_key const & privatekey_a)
{
	chratos::uint256_union result;
	ed25519_publickey (privatekey_a.bytes.data (), result.bytes.data ());
	return result;
}

bool chratos::validate_message (chratos::public_key const & public_key, chratos::uint256_union const & message, chratos::uint512_union const & signature)
{
	auto result (0 != ed25519_sign_open (message.bytes.data (), sizeof (message.bytes), public_key.bytes.data (), signature.bytes.data ()));
	return result;
}

chratos::uint128_union::uint128_union (std::string const & string_a)
{
	decode_hex (string_a);
}

chratos::uint128_union::uint128_union (uint64_t value_a)
{
	*this = chratos::uint128_t (value_a);
}

chratos::uint128_union::uint128_union (chratos::uint128_t const & value_a)
{
	chratos::uint128_t number_l (value_a);
	for (auto i (bytes.rbegin ()), n (bytes.rend ()); i != n; ++i)
	{
		*i = static_cast<uint8_t> (number_l & static_cast<uint8_t> (0xff));
		number_l >>= 8;
	}
}

bool chratos::uint128_union::operator== (chratos::uint128_union const & other_a) const
{
	return qwords[0] == other_a.qwords[0] && qwords[1] == other_a.qwords[1];
}

bool chratos::uint128_union::operator!= (chratos::uint128_union const & other_a) const
{
	return !(*this == other_a);
}

bool chratos::uint128_union::operator< (chratos::uint128_union const & other_a) const
{
	return number () < other_a.number ();
}

bool chratos::uint128_union::operator> (chratos::uint128_union const & other_a) const
{
	return number () > other_a.number ();
}

chratos::uint128_t chratos::uint128_union::number () const
{
	chratos::uint128_t result;
	auto shift (0);
	for (auto i (bytes.begin ()), n (bytes.end ()); i != n; ++i)
	{
		result <<= shift;
		result |= *i;
		shift = 8;
	}
	return result;
}

void chratos::uint128_union::encode_hex (std::string & text) const
{
	assert (text.empty ());
	std::stringstream stream;
	stream << std::hex << std::noshowbase << std::setw (32) << std::setfill ('0');
	stream << number ();
	text = stream.str ();
}

bool chratos::uint128_union::decode_hex (std::string const & text)
{
	auto error (text.size () > 32);
	if (!error)
	{
		std::stringstream stream (text);
		stream << std::hex << std::noshowbase;
		chratos::uint128_t number_l;
		try
		{
			stream >> number_l;
			*this = number_l;
			if (!stream.eof ())
			{
				error = true;
			}
		}
		catch (std::runtime_error &)
		{
			error = true;
		}
	}
	return error;
}

void chratos::uint128_union::encode_dec (std::string & text) const
{
	assert (text.empty ());
	std::stringstream stream;
	stream << std::dec << std::noshowbase;
	stream << number ();
	text = stream.str ();
}

bool chratos::uint128_union::decode_dec (std::string const & text)
{
	auto error (text.size () > 39 || (text.size () > 1 && text[0] == '0') || (text.size () > 0 && text[0] == '-'));
	if (!error)
	{
		std::stringstream stream (text);
		stream << std::dec << std::noshowbase;
		boost::multiprecision::checked_uint128_t number_l;
		try
		{
			stream >> number_l;
			chratos::uint128_t unchecked (number_l);
			*this = unchecked;
			if (!stream.eof ())
			{
				error = true;
			}
		}
		catch (std::runtime_error &)
		{
			error = true;
		}
	}
	return error;
}

void format_frac (std::ostringstream & stream, chratos::uint128_t value, chratos::uint128_t scale, int precision)
{
	auto reduce = scale;
	auto rem = value;
	while (reduce > 1 && rem > 0 && precision > 0)
	{
		reduce /= 10;
		auto val = rem / reduce;
		rem -= val * reduce;
		stream << val;
		precision--;
	}
}

void format_dec (std::ostringstream & stream, chratos::uint128_t value, char group_sep, const std::string & groupings)
{
	auto largestPow10 = chratos::uint256_t (1);
	int dec_count = 1;
	while (1)
	{
		auto next = largestPow10 * 10;
		if (next > value)
		{
			break;
		}
		largestPow10 = next;
		dec_count++;
	}

	if (dec_count > 39)
	{
		// Impossible.
		return;
	}

	// This could be cached per-locale.
	bool emit_group[39];
	if (group_sep != 0)
	{
		int group_index = 0;
		int group_count = 0;
		for (int i = 0; i < dec_count; i++)
		{
			group_count++;
			if (group_count > groupings[group_index])
			{
				group_index = std::min (group_index + 1, (int)groupings.length () - 1);
				group_count = 1;
				emit_group[i] = true;
			}
			else
			{
				emit_group[i] = false;
			}
		}
	}

	auto reduce = chratos::uint128_t (largestPow10);
	chratos::uint128_t rem = value;
	while (reduce > 0)
	{
		auto val = rem / reduce;
		rem -= val * reduce;
		stream << val;
		dec_count--;
		if (group_sep != 0 && emit_group[dec_count] && reduce > 1)
		{
			stream << group_sep;
		}
		reduce /= 10;
	}
}

std::string format_balance (chratos::uint128_t balance, chratos::uint128_t scale, int precision, bool group_digits, char thousands_sep, char decimal_point, std::string & grouping)
{
	std::ostringstream stream;
	auto int_part = balance / scale;
	auto frac_part = balance % scale;
	auto prec_scale = scale;
	for (int i = 0; i < precision; i++)
	{
		prec_scale /= 10;
	}
	if (int_part == 0 && frac_part > 0 && frac_part / prec_scale == 0)
	{
		// Display e.g. "< 0.01" rather than 0.
		stream << "< ";
		if (precision > 0)
		{
			stream << "0";
			stream << decimal_point;
			for (int i = 0; i < precision - 1; i++)
			{
				stream << "0";
			}
		}
		stream << "1";
	}
	else
	{
		format_dec (stream, int_part, group_digits && grouping.length () > 0 ? thousands_sep : 0, grouping);
		if (precision > 0 && frac_part > 0)
		{
			stream << decimal_point;
			format_frac (stream, frac_part, scale, precision);
		}
	}
	return stream.str ();
}

std::string chratos::uint128_union::format_balance (chratos::uint128_t scale, int precision, bool group_digits)
{
	auto thousands_sep = std::use_facet<std::numpunct<char>> (std::locale ()).thousands_sep ();
	auto decimal_point = std::use_facet<std::numpunct<char>> (std::locale ()).decimal_point ();
	std::string grouping = "\3";
	return ::format_balance (number (), scale, precision, group_digits, thousands_sep, decimal_point, grouping);
}

std::string chratos::uint128_union::format_balance (chratos::uint128_t scale, int precision, bool group_digits, const std::locale & locale)
{
	auto thousands_sep = std::use_facet<std::moneypunct<char>> (locale).thousands_sep ();
	auto decimal_point = std::use_facet<std::moneypunct<char>> (locale).decimal_point ();
	std::string grouping = std::use_facet<std::moneypunct<char>> (locale).grouping ();
	return ::format_balance (number (), scale, precision, group_digits, thousands_sep, decimal_point, grouping);
}

void chratos::uint128_union::clear ()
{
	qwords.fill (0);
}

bool chratos::uint128_union::is_zero () const
{
	return qwords[0] == 0 && qwords[1] == 0;
}

std::string chratos::uint128_union::to_string () const
{
	std::string result;
	encode_hex (result);
	return result;
}

std::string chratos::uint128_union::to_string_dec () const
{
	std::string result;
	encode_dec (result);
	return result;
}