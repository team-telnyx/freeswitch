/**********************************************

License: BSD
Project Webpage: http://cajun-jsonapi.sourceforge.net/
Author: Terry Caton

***********************************************/

#include <cassert>
#include <set>
#include <sstream>

/*  

TODO:
* better documentation

*/

namespace json
{


   inline std::istream& operator >> (std::istream& istr, UnknownElement& elementRoot) {
   Reader::Read(elementRoot, istr);
   return istr;
}

inline Reader::Location::Location() :
   m_nLine(0),
   m_nLineOffset(0),
   m_nDocOffset(0)
{}


//////////////////////
// Reader::InputStream

class Reader::InputStream // would be cool if we could inherit from std::istream & override "get"
{
public:
   InputStream(std::istream& iStr) :
      m_iStr(iStr) {}

   // protect access to the input stream, so we can keeep track of document/line offsets
   char Get(); // big, define outside
   char Peek() {
      assert(m_iStr.eof() == false); // enforce reading of only valid stream data 
      return m_iStr.peek();
   }

   bool EOS() {
      m_iStr.peek(); // apparently eof flag isn't set until a character read is attempted. whatever.
      return m_iStr.eof();
   }

   const Location& GetLocation() const { return m_Location; }

private:
   std::istream& m_iStr;
   Location m_Location;
};


inline char Reader::InputStream::Get()
{
   assert(m_iStr.eof() == false); // enforce reading of only valid stream data 
   char c = m_iStr.get();
   
   ++m_Location.m_nDocOffset;
   if (c == '\n') {
      ++m_Location.m_nLine;
      m_Location.m_nLineOffset = 0;
   }
   else {
      ++m_Location.m_nLineOffset;
   }

   return c;
}



//////////////////////
// Reader::TokenStream

class Reader::TokenStream
{
public:
   TokenStream(const Tokens& tokens);

   const Token& Peek();
   const Token& Get();

   bool EOS() const;

private:
   const Tokens& m_Tokens;
   Tokens::const_iterator m_itCurrent;
};


inline Reader::TokenStream::TokenStream(const Tokens& tokens) :
   m_Tokens(tokens),
   m_itCurrent(tokens.begin())
{}

inline const Reader::Token& Reader::TokenStream::Peek() {
   assert(m_itCurrent != m_Tokens.end());
   return *(m_itCurrent); 
}

inline const Reader::Token& Reader::TokenStream::Get() {
   assert(m_itCurrent != m_Tokens.end());
   return *(m_itCurrent++); 
}

inline bool Reader::TokenStream::EOS() const {
   return m_itCurrent == m_Tokens.end(); 
}

///////////////////
// Reader (finally)


inline void Reader::Read(Object& object, std::istream& istr)                { Read_i(object, istr); }
inline void Reader::Read(Array& array, std::istream& istr)                  { Read_i(array, istr); }
inline void Reader::Read(String& string, std::istream& istr)                { Read_i(string, istr); }
inline void Reader::Read(Number& number, std::istream& istr)                { Read_i(number, istr); }
inline void Reader::Read(Boolean& boolean, std::istream& istr)              { Read_i(boolean, istr); }
inline void Reader::Read(Null& null, std::istream& istr)                    { Read_i(null, istr); }
inline void Reader::Read(UnknownElement& unknown, std::istream& istr)       { Read_i(unknown, istr); }


template <typename ElementTypeT>   
void Reader::Read_i(ElementTypeT& element, std::istream& istr)
{
   Reader reader;

   Tokens tokens;
   InputStream inputStream(istr);
   reader.Scan(tokens, inputStream);

   TokenStream tokenStream(tokens);
   reader.Parse(element, tokenStream);

   if (tokenStream.EOS() == false)
   {
      const Token& token = tokenStream.Peek();
      std::string sMessage = "Expected End of token stream; found " + token.sValue;
      throw ParseException(sMessage, token.locBegin, token.locEnd);
   }
}


inline void Reader::Scan(Tokens& tokens, InputStream& inputStream)
{
   while (EatWhiteSpace(inputStream),              // ignore any leading white space...
          inputStream.EOS() == false) // ...before checking for EOS
   {
      // if all goes well, we'll create a token each pass
      Token token;
      token.locBegin = inputStream.GetLocation();

      // gives us null-terminated string
      std::string sChar;
      sChar.push_back(inputStream.Peek());

      switch (sChar[0])
      {
         case '{':
            token.sValue = sChar[0];
            MatchExpectedString(sChar, inputStream);
            token.nType = Token::TOKEN_OBJECT_BEGIN;
            break;

         case '}':
            token.sValue = sChar[0];
            MatchExpectedString(sChar, inputStream);
            token.nType = Token::TOKEN_OBJECT_END;
            break;

         case '[':
            token.sValue = sChar[0];
            MatchExpectedString(sChar, inputStream);
            token.nType = Token::TOKEN_ARRAY_BEGIN;
            break;

         case ']':
            token.sValue = sChar[0];
            MatchExpectedString(sChar, inputStream);
            token.nType = Token::TOKEN_ARRAY_END;
            break;

         case ',':
            token.sValue = sChar[0];
            MatchExpectedString(sChar, inputStream);
            token.nType = Token::TOKEN_NEXT_ELEMENT;
            break;

         case ':':
            token.sValue = sChar[0];
            MatchExpectedString(sChar, inputStream);
            token.nType = Token::TOKEN_MEMBER_ASSIGN;
            break;

         case '"':
            MatchString(token.sValue, inputStream);
            token.nType = Token::TOKEN_STRING;
            break;

         case '-':
         case '0':
         case '1':
         case '2':
         case '3':
         case '4':
         case '5':
         case '6':
         case '7':
         case '8':
         case '9':
            MatchNumber(token.sValue, inputStream);
            token.nType = Token::TOKEN_NUMBER;
            break;

         case 't':
            token.sValue = "true";
            MatchExpectedString(token.sValue, inputStream);
            token.nType = Token::TOKEN_BOOLEAN;
            break;

         case 'f':
            token.sValue = "false";
            MatchExpectedString(token.sValue, inputStream);
            token.nType = Token::TOKEN_BOOLEAN;
            break;

         case 'n':
            token.sValue = "null";
            MatchExpectedString(token.sValue, inputStream);
            token.nType = Token::TOKEN_NULL;
            break;

         default: {
            std::string sErrorMessage = "Unexpected character in stream: " + sChar;
            throw ScanException(sErrorMessage, inputStream.GetLocation());
         }
      }

      token.locEnd = inputStream.GetLocation();
      tokens.push_back(token);
   }
}


inline void Reader::EatWhiteSpace(InputStream& inputStream)
{
   while (inputStream.EOS() == false && 
          ::isspace(inputStream.Peek()))
      inputStream.Get();
}

inline void Reader::MatchExpectedString(const std::string& sExpected, InputStream& inputStream)
{
   std::string::const_iterator it(sExpected.begin()),
                               itEnd(sExpected.end());
   for ( ; it != itEnd; ++it) {
      if (inputStream.EOS() ||      // did we reach the end before finding what we're looking for...
          inputStream.Get() != *it) // ...or did we find something different?
      {
         std::string sMessage = "Expected string: " + sExpected;
         throw ScanException(sMessage, inputStream.GetLocation());
      }
   }

   // all's well if we made it here, return quietly
}


// Telnyx fork of upstream CAJUN. Upstream left \u unimplemented -- the case
// label fell through to the throwing default -- so a document carrying one was
// refused whatever it decoded to, which is a legal document every conforming
// parser reads.

inline int Utf16HexDigit(char c)
{
   if (c >= '0' && c <= '9') { return c - '0'; }
   if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
   if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
   return -1;
}

inline void AppendUtf8(std::string& out, unsigned long cp)
{
   if (cp < 0x80UL) {
      out.push_back(static_cast<char>(cp));
   }
   else if (cp < 0x800UL) {
      out.push_back(static_cast<char>(0xc0UL | (cp >> 6)));
      out.push_back(static_cast<char>(0x80UL | (cp & 0x3fUL)));
   }
   else if (cp < 0x10000UL) {
      out.push_back(static_cast<char>(0xe0UL | (cp >> 12)));
      out.push_back(static_cast<char>(0x80UL | ((cp >> 6) & 0x3fUL)));
      out.push_back(static_cast<char>(0x80UL | (cp & 0x3fUL)));
   }
   else {
      out.push_back(static_cast<char>(0xf0UL | (cp >> 18)));
      out.push_back(static_cast<char>(0x80UL | ((cp >> 12) & 0x3fUL)));
      out.push_back(static_cast<char>(0x80UL | ((cp >> 6) & 0x3fUL)));
      out.push_back(static_cast<char>(0x80UL | (cp & 0x3fUL)));
   }
}

inline unsigned long Reader::MatchHex4(InputStream& inputStream)
{
   unsigned long value = 0;
   for (int i = 0; i < 4; ++i) {
      if (inputStream.EOS()) {
         throw ScanException("Truncated \\u escape sequence in string",
                             inputStream.GetLocation());
      }
      const int digit = Utf16HexDigit(inputStream.Get());
      if (digit < 0) {
         throw ScanException("Non-hex digit in \\u escape sequence in string",
                             inputStream.GetLocation());
      }
      value = (value << 4) | static_cast<unsigned long>(digit);
   }
   return value;
}

inline void Reader::MatchString(std::string& string, InputStream& inputStream)
{
   MatchExpectedString("\"", inputStream);
   
   while (inputStream.EOS() == false &&
          inputStream.Peek() != '"')
   {
      char c = inputStream.Get();

      // escape?
      if (c == '\\' &&
          inputStream.EOS() == false) // shouldn't have reached the end yet
      {
         c = inputStream.Get();
         switch (c) {
            case '/':      string.push_back('/');     break;
            case '"':      string.push_back('"');     break;
            case '\\':     string.push_back('\\');    break;
            case 'b':      string.push_back('\b');    break;
            case 'f':      string.push_back('\f');    break;
            case 'n':      string.push_back('\n');    break;
            case 'r':      string.push_back('\r');    break;
            case 't':      string.push_back('\t');    break;
            case 'u': {
               unsigned long cp = MatchHex4(inputStream);
               // A high surrogate is half a character. JSON spells anything
               // above the BMP as a pair, so the low half has to follow or
               // what we append is not the character that was written.
               if (cp >= 0xd800UL && cp <= 0xdbffUL) {
                  if (inputStream.EOS() || inputStream.Get() != '\\' ||
                      inputStream.EOS() || inputStream.Get() != 'u') {
                     throw ScanException("Unpaired UTF-16 high surrogate in string",
                                         inputStream.GetLocation());
                  }
                  const unsigned long low = MatchHex4(inputStream);
                  if (low < 0xdc00UL || low > 0xdfffUL) {
                     throw ScanException("Invalid UTF-16 low surrogate in string",
                                         inputStream.GetLocation());
                  }
                  cp = 0x10000UL + ((cp - 0xd800UL) << 10) + (low - 0xdc00UL);
               }
               else if (cp >= 0xdc00UL && cp <= 0xdfffUL) {
                  throw ScanException("Unpaired UTF-16 low surrogate in string",
                                      inputStream.GetLocation());
               }
               AppendUtf8(string, cp);
               break;
            }
            default: {
               std::string sMessage = "Unrecognized escape sequence found in string: \\";
               sMessage.push_back(c);
               throw ScanException(sMessage, inputStream.GetLocation());
            }
         }
      }
      else {
         string.push_back(c);
      }
   }

   // eat the last '"' that we just peeked
   MatchExpectedString("\"", inputStream);
}


inline void Reader::MatchNumber(std::string& sNumber, InputStream& inputStream)
{
   const char sNumericChars[] = "0123456789.eE-+";
   std::set<char> numericChars;
   numericChars.insert(sNumericChars, sNumericChars + sizeof(sNumericChars));

   while (inputStream.EOS() == false &&
          numericChars.find(inputStream.Peek()) != numericChars.end())
   {
      sNumber.push_back(inputStream.Get());   
   }
}


inline void Reader::Parse(UnknownElement& element, Reader::TokenStream& tokenStream) 
{
   if (tokenStream.EOS()) {
      std::string sMessage = "Unexpected end of token stream";
      throw ParseException(sMessage, Location(), Location()); // nowhere to point to
   }

   const Token& token = tokenStream.Peek();
   switch (token.nType) {
      case Token::TOKEN_OBJECT_BEGIN:
      {
         // implicit non-const cast will perform conversion for us (if necessary)
         Object& object = element;
         Parse(object, tokenStream);
         break;
      }

      case Token::TOKEN_ARRAY_BEGIN:
      {
         Array& array = element;
         Parse(array, tokenStream);
         break;
      }

      case Token::TOKEN_STRING:
      {
         String& string = element;
         Parse(string, tokenStream);
         break;
      }

      case Token::TOKEN_NUMBER:
      {
         Number& number = element;
         Parse(number, tokenStream);
         break;
      }

      case Token::TOKEN_BOOLEAN:
      {
         Boolean& boolean = element;
         Parse(boolean, tokenStream);
         break;
      }

      case Token::TOKEN_NULL:
      {
         Null& null = element;
         Parse(null, tokenStream);
         break;
      }

      default:
      {
         std::string sMessage = "Unexpected token: " + token.sValue;
         throw ParseException(sMessage, token.locBegin, token.locEnd);
      }
   }
}


inline void Reader::Parse(Object& object, Reader::TokenStream& tokenStream)
{
   MatchExpectedToken(Token::TOKEN_OBJECT_BEGIN, tokenStream);

   bool bContinue = (tokenStream.EOS() == false &&
                     tokenStream.Peek().nType != Token::TOKEN_OBJECT_END);
   while (bContinue)
   {
      Object::Member member;

      // first the member name. save the token in case we have to throw an exception
      const Token& tokenName = tokenStream.Peek();
      member.name = MatchExpectedToken(Token::TOKEN_STRING, tokenStream);

      // ...then the key/value separator...
      MatchExpectedToken(Token::TOKEN_MEMBER_ASSIGN, tokenStream);

      // ...then the value itself (can be anything).
      Parse(member.element, tokenStream);

      // try adding it to the object (this could throw)
      try
      {
         object.Insert(member);
      }
      catch (Exception&)
      {
         // must be a duplicate name
         std::string sMessage = "Duplicate object member token: " + member.name; 
         throw ParseException(sMessage, tokenName.locBegin, tokenName.locEnd);
      }

      bContinue = (tokenStream.EOS() == false &&
                   tokenStream.Peek().nType == Token::TOKEN_NEXT_ELEMENT);
      if (bContinue)
         MatchExpectedToken(Token::TOKEN_NEXT_ELEMENT, tokenStream);
   }

   MatchExpectedToken(Token::TOKEN_OBJECT_END, tokenStream);
}


inline void Reader::Parse(Array& array, Reader::TokenStream& tokenStream)
{
   MatchExpectedToken(Token::TOKEN_ARRAY_BEGIN, tokenStream);

   bool bContinue = (tokenStream.EOS() == false &&
                     tokenStream.Peek().nType != Token::TOKEN_ARRAY_END);
   while (bContinue)
   {
      // ...what's next? could be anything
      Array::iterator itElement = array.Insert(UnknownElement());
      UnknownElement& element = *itElement;
      Parse(element, tokenStream);

      bContinue = (tokenStream.EOS() == false &&
                   tokenStream.Peek().nType == Token::TOKEN_NEXT_ELEMENT);
      if (bContinue)
         MatchExpectedToken(Token::TOKEN_NEXT_ELEMENT, tokenStream);
   }

   MatchExpectedToken(Token::TOKEN_ARRAY_END, tokenStream);
}


inline void Reader::Parse(String& string, Reader::TokenStream& tokenStream)
{
   string = MatchExpectedToken(Token::TOKEN_STRING, tokenStream);
}


inline void Reader::Parse(Number& number, Reader::TokenStream& tokenStream)
{
   const Token& currentToken = tokenStream.Peek(); // might need this later for throwing exception
   const std::string& sValue = MatchExpectedToken(Token::TOKEN_NUMBER, tokenStream);

   std::istringstream iStr(sValue);
   double dValue;
   iStr >> dValue;

   // did we consume all characters in the token?
   if (iStr.eof() == false)
   {
      std::string sMessage = "Unexpected character in NUMBER";
      throw ParseException(sMessage, currentToken.locBegin, currentToken.locEnd);
   }

   number = dValue;
}


inline void Reader::Parse(Boolean& boolean, Reader::TokenStream& tokenStream)
{
   const std::string& sValue = MatchExpectedToken(Token::TOKEN_BOOLEAN, tokenStream);
   boolean = (sValue == "true" ? true : false);
}


inline void Reader::Parse(Null&, Reader::TokenStream& tokenStream)
{
   MatchExpectedToken(Token::TOKEN_NULL, tokenStream);
}


inline const std::string& Reader::MatchExpectedToken(Token::Type nExpected, Reader::TokenStream& tokenStream)
{
   if (tokenStream.EOS())
   {
      std::string sMessage = "Unexpected End of token stream";
      throw ParseException(sMessage, Location(), Location()); // nowhere to point to
   }

   const Token& token = tokenStream.Get();
   if (token.nType != nExpected)
   {
      std::string sMessage = "Unexpected token: " + token.sValue;
      throw ParseException(sMessage, token.locBegin, token.locEnd);
   }

   return token.sValue;
}

} // End namespace
