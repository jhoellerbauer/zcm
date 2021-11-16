#include "Common.hpp"
#include "Emitter.hpp"
#include "GetOpt.hpp"
#include "ZCMGen.hpp"
#include "util/StringUtil.hpp"
#include "util/FileUtil.hpp"
using std::to_string;

#include <vector>
using std::vector;

#include <iostream>
using std::cerr;

void setupOptionsPython(GetOpt& gopt)
{
    gopt.addString(0, "ppath", "./", "Python destination directory");
}

static string getStructFormat(const ZCMMember& zm)
{
    auto& tn = zm.type.fullname;

    // Handle bitfields
    if (zm.type.numbits != 0) {
        string ret = zm.type.signExtend ? "s" : "u";
        ret += to_string(zm.type.numbits);
        return ret;
    }

    if (tn == "byte")    return "B";
    if (tn == "boolean") return "b";
    if (tn == "int8_t")  return "b";
    if (tn == "int16_t") return "h";
    if (tn == "int32_t") return "i";
    if (tn == "int64_t") return "q";
    if (tn == "float")   return "f";
    if (tn == "double")  return "d";
    return "";
}

struct PyEmitStruct : public Emitter
{
    const ZCMGen& zcm;
    const ZCMStruct& zs;

    PyEmitStruct(const ZCMGen& zcm, const ZCMStruct& zs, const string& fname):
        Emitter(fname), zcm(zcm), zs(zs) {}

    void emitStruct()
    {
        auto& sn_ = zs.structname.shortname;
        auto* sn = sn_.c_str();

        emit(0, "\"\"\"ZCM type definitions\n"
             "This file automatically generated by zcm.\n"
             "DO NOT MODIFY BY HAND!!!!\n"
             "\"\"\"\n"
             "\n"
             "try:\n"
             "    import cStringIO.StringIO as BytesIO\n"
             "except ImportError:\n"
             "    from io import BytesIO\n"
             "import struct");
        for (auto& zm : zs.members) {
            if (zm.type.numbits != 0) {
                emit(0, "import os, math, bitstruct");
                break;
            }
        }
        emit(0, "");

        emitPythonDependencies();

        emit(0, "class %s(object):", sn);
        emitStart(0, "    __slots__ = [");
        for (size_t m = 0; m < zs.members.size(); ++m) {
            auto& zm = zs.members[m];
            emitContinue("\"%s\"%s", zm.membername.c_str(),
                         (m < zs.members.size()-1) ? ", " : "");
        }
        emitEnd("]");
        emit(0, "");

        // CONSTANTS
        emit(1, "IS_LITTLE_ENDIAN = %s;",
                zcm.gopt->getBool("little-endian-encoding") ? "True" : "False");
        for (auto& zc : zs.constants) {
            static string hexPrefix = "0x";
            bool isHex = zc.valstr.size() > 2 &&
                         zc.valstr.compare(0, hexPrefix.length(), hexPrefix) == 0;
            bool isNeg = zc.val.i64 < 0;
            if (isHex && !isNeg) {
                emit(1, "%s = %s;", zc.membername.c_str(), zc.valstr.c_str());
            } else {
                emit(1, "%s = %lld;", zc.membername.c_str(), zc.val.i64);
            }
        }
        if (zs.constants.size() > 0)
            emit(0, "");

        emitPythonInit();
        emitPythonEncode();
        emitPythonEncodeOne();
        emitPythonDecode();
        emitPythonDecodeOne();
        emitPythonFingerprint();
    }

    void emitDecodeOne(const ZCMMember& zm, const string& accessor_, int indent, const string& sfx_)
    {
        auto& tn = zm.type.fullname;
        auto& mn = zm.membername;
        auto& sn = zm.type.shortname;

        auto* accessor = accessor_.c_str();
        auto* sfx = sfx_.c_str();

        if (tn == "string") {
            emit(indent, "__%s_len = struct.unpack('>I', buf.read(4))[0]", mn.c_str());
            emit(indent, "%sbuf.read(__%s_len)[:-1].decode('utf-8', 'replace')%s",
                 accessor, mn.c_str(), sfx);
        } else if (tn == "boolean") {
            emit(indent, "%sbool(struct.unpack('b', buf.read(1))[0])%s", accessor, sfx);
        } else if (tn == zs.structname.fullname) {
            emit(indent, "%s%s._decode_one(buf)%s", accessor, sn.c_str(), sfx);
        } else {
            emit(indent, "%s%s._decode_one(buf)%s",
                         accessor, zm.type.nameUnderscoreCStr(), sfx);
        }
    }

    void emitDecodeList(const ZCMMember& zm, const string& accessor_, int indent, bool isFirst,
                        const string& len_, bool fixedLen)
    {
        auto& tn = zm.type.fullname;
        const char* suffix = isFirst ? "" : ")";
        auto* accessor = accessor_.c_str();
        auto* len = len_.c_str();

        if (zm.type.numbits != 0) {
            emit(indent, "numbits = %s%s * %u + offset_bit", fixedLen ? "" : "self.", len, zm.type.numbits);
            emit(indent, "bitbuf = buf.read(math.ceil(numbits / 8))");
            emit(indent, "formatstr = %s%s * \"%s\"", fixedLen ? "" : "self.", len, getStructFormat(zm).c_str());
            emit(indent, "%s[*bitstruct.unpack_from('>' + formatstr + '>', bitbuf, offset_bit)]%s",
                 accessor, suffix);
        } else if (tn == "byte") {
            emit(indent, "%sbuf.read(%s%s)%s",
                  accessor, fixedLen ? "" : "self.", len, suffix);
        } else if (tn == "boolean") {
            if(fixedLen) {
                emit(indent, "%smap(bool, struct.unpack('>%s%s', buf.read(%d)))%s",
                     accessor, len, getStructFormat(zm).c_str(),
                     atoi(len) * ZCMGen::getPrimitiveTypeSize(tn),
                     suffix);
            } else {
                emit(indent,
                     "%smap(bool, struct.unpack('>%%d%s' %% self.%s, buf.read(self.%s)))%s",
                     accessor, getStructFormat(zm).c_str(), len, len, suffix);
            }
        } else if (tn == "int8_t" || tn == "int16_t" || tn == "int32_t" || tn == "int64_t" ||
                   tn == "float"  || tn == "double") {
            if (fixedLen) {
                emit(indent, "%sstruct.unpack('>%s%s', buf.read(%d))%s",
                     accessor, len, getStructFormat(zm).c_str(),
                     atoi(len) * ZCMGen::getPrimitiveTypeSize(tn),
                     suffix);
            } else {
                if (ZCMGen::getPrimitiveTypeSize(tn) > 1) {
                    emit(indent, "%sstruct.unpack('>%%d%s' %% self.%s, buf.read(self.%s * %d))%s",
                         accessor, getStructFormat(zm).c_str(), len, len,
                         ZCMGen::getPrimitiveTypeSize(tn), suffix);
                } else {
                    emit(indent, "%sstruct.unpack('>%%d%s' %% self.%s, buf.read(self.%s))%s",
                         accessor, getStructFormat(zm).c_str(), len, len, suffix);
                }
            }
        } else {
            assert(0);
        }
    }

    void flushReadStructFmt(vector<string>& formats, vector<const ZCMMember*>& members)
    {
        size_t nfmts = formats.size();
        assert(nfmts == members.size());
        if (nfmts == 0) return;

        if (members[0]->type.numbits != 0) {
            size_t numbits = 0;
            for (const auto* zm : members) numbits += zm->type.numbits;
            emit(2, "numbits = %u + offset_bit", numbits);
            emit(2, "bitbuf = buf.read(math.ceil(numbits / 8))");
            emitStart(2, "");
            for (size_t i = 0; i < members.size(); ++i) {
                const auto* zm = members[i];
                emitContinue("self.%s", zm->membername.c_str());
                if (i != members.size() - 1) emitContinue (", ");
            }
            emitContinue (" = bitstruct.unpack_from(\">");
            for (const auto& f : formats) emitContinue("%s", f.c_str());
            emitEnd(">\", bitbuf, offset_bit)%s", nfmts == 1 ? "[0]" : "");
        } else {
            emitStart(0, "        ");
            int fmtsize = 0;
            for (size_t i = 0; i < members.size(); ++i) {
                const auto* zm = members[i];
                emitContinue("self.%s", zm->membername.c_str());
                if (i != members.size() - 1) emitContinue (", ");
                fmtsize += ZCMGen::getPrimitiveTypeSize(zm->type.fullname);
            }
            emitContinue (" = struct.unpack(\">");
            for (const auto& f : formats) emitContinue("%s", f.c_str());
            emitEnd("\", buf.read(%d))%s", fmtsize, nfmts == 1 ? "[0]" : "");
        }

        formats.clear();
        members.clear();
    }

    void emitPythonDecodeOne()
    {
        emit(1, "def _decode_one(buf):");
        emit(2, "self = %s()", zs.structname.shortname.c_str());

        vector<string> structFmt;
        vector<const ZCMMember*> structMembers;

        bool inBitMode = false;
        size_t bitfieldNum = 0;
        for (auto& zm : zs.members) {

            string fmt = getStructFormat(zm);
            bool isolate = fmt.empty() || zm.type.fullname == "boolean";

            if (!inBitMode && zm.type.numbits != 0) {
                if (!structFmt.empty()) {
                    flushReadStructFmt(structFmt, structMembers);
                }
                inBitMode = true;
                emit(0, "");
                emit(2, "# Start of bitfield %u", bitfieldNum);
                emit(0, "");
                emit(2, "offset_bit = 0;");
                emit(0, "");
            } else if (inBitMode && zm.type.numbits == 0) {
                if (!structFmt.empty()) {
                    flushReadStructFmt(structFmt, structMembers);
                } else {
                    emit(2, "if (offset_bit != 0):");
                    emit(3, "buf.seek(1, os.SEEK_CUR)");
                }
                inBitMode = false;
                emit(0, "");
                emit(2, "# End of bitfield %u", bitfieldNum);
                emit(0, "");
                ++bitfieldNum;
            }

            if (zm.dimensions.size() == 0) {
                if (isolate) {
                    string accessor = "self." + zm.membername + " = ";
                    emitDecodeOne(zm, accessor.c_str(), 2, "");
                } else {
                    structFmt.push_back(fmt);
                    structMembers.push_back(&zm);
                }
            } else {
                if (!structFmt.empty()) {
                    flushReadStructFmt(structFmt, structMembers);
                    if (inBitMode) {
                        emit(2, "offset_bit = numbits %% 8");
                        emit(2, "if (offset_bit != 0):");
                        emit(3, "buf.seek(-1, os.SEEK_CUR)");
                        emit(0, "");
                    }
                }

                string accessor = "self." + zm.membername;

                // iterate through the dimensions of the member, building up
                // an accessor string, and emitting for loops
                size_t n = 0;
                for (; n < zm.dimensions.size()-1; ++n) {
                    auto& dim = zm.dimensions[n];

                    if(n == 0) {
                        emit(2, "%s = []", accessor.c_str());
                    } else {
                        emit(2+n, "%s.append([])", accessor.c_str());
                    }

                    if (dim.mode == ZCM_CONST) {
                        emit(2+n, "for i%d in range(%s):", n, dim.size.c_str());
                    } else {
                        emit(2+n, "for i%d in range(self.%s):", n, dim.size.c_str());
                    }

                    if(n > 0 && n < zm.dimensions.size()-1) {
                        accessor += "[i" + to_string(n-1) + "]";
                    }
                }

                // last dimension.
                auto& lastDim = zm.dimensions[zm.dimensions.size()-1];
                bool lastDimFixedLen = (lastDim.mode == ZCM_CONST);

                if (ZCMGen::isPrimitiveType(zm.type.fullname) &&
                    zm.type.fullname != "string") {
                    // member is a primitive non-string type.  Emit code to
                    // decode a full array in one call to struct.unpack
                    if(n == 0) {
                        accessor += " = ";
                    } else {
                        accessor += ".append(";
                    }

                    emitDecodeList(zm, accessor, 2+n, n==0,
                                   lastDim.size, lastDimFixedLen);
                    if (inBitMode) {
                        emit(2+n, "offset_bit = numbits %% 8");
                        emit(2+n, "if (offset_bit != 0):");
                        emit(3+n, "buf.seek(-1, os.SEEK_CUR)");
                        emit(0, "");
                    }
                } else {
                    // member is either a string type or an inner ZCM type.  Each
                    // array element must be decoded individually
                    if(n == 0) {
                        emit(2, "%s = []", accessor.c_str());
                    } else {
                        emit(2+n, "%s.append ([])", accessor.c_str());
                        accessor += "[i" + to_string(n-1) + "]";
                    }
                    if (lastDimFixedLen) {
                        emit(2+n, "for i%d in range(%s):", n, lastDim.size.c_str());
                    } else {
                        emit(2+n, "for i%d in range(self.%s):", n, lastDim.size.c_str());
                    }
                    accessor += ".append(";
                    emitDecodeOne(zm, accessor, n+3, ")");
                    if (inBitMode) {
                        emit(2+n, "offset_bit = numbits %% 8");
                        emit(2+n, "if (offset_bit != 0):");
                        emit(3+n, "buf.seek(-1, os.SEEK_CUR)");
                        emit(0, "");
                    }
                }
            }
        }
        flushReadStructFmt(structFmt, structMembers);
        if (inBitMode) {
            emit(2, "# End of bitfield %u", bitfieldNum);
            emit(0, "");
        }
        emit(2, "return self");

        emit(1, "_decode_one = staticmethod(_decode_one)");
        emit(0, "");
    }

    void emitPythonDecode()
    {
        emit(1, "def decode(data):");
        emit(1, "    if hasattr(data, 'read'):");
        emit(1, "        buf = data");
        emit(1, "    else:");
        emit(1, "        buf = BytesIO(data)");
        emit(1, "    if buf.read(8) != %s._get_packed_fingerprint():", zs.structname.shortname.c_str());
        emit(1, "        raise ValueError(\"Decode error\")");
        emit(1, "    return %s._decode_one(buf)", zs.structname.shortname.c_str());
        emit(1, "decode = staticmethod(decode)");
        emit(0, "");
    }

    void emitEncodeOne(const ZCMMember& zm, const string& accessor_, int indent)
    {
        const string& tn = zm.type.fullname;
        const string& mn = zm.membername;
        auto* accessor = accessor_.c_str();

        if (tn == "string") {
            emit(indent, "__%s_encoded = %s.encode('utf-8')", mn.c_str(), accessor);
            emit(indent, "buf.write(struct.pack('>I', len(__%s_encoded)+1))", mn.c_str());
            emit(indent, "buf.write(__%s_encoded)", mn.c_str());
            emit(indent, "buf.write(b\"\\0\")");
        } else if (tn == "byte") {
            emit(indent, "buf.write(struct.pack('B', %s))", accessor);
        } else if (tn == "int8_t" || tn == "boolean") {
            emit(indent, "buf.write(struct.pack('b', %s))", accessor);
        } else if (tn == "int16_t") {
            emit(indent, "buf.write(struct.pack('>h', %s))", accessor);
        } else if (tn == "int32_t") {
            emit(indent, "buf.write(struct.pack('>i', %s))", accessor);
        } else if (tn == "int64_t") {
            emit(indent, "buf.write(struct.pack('>q', %s))", accessor);
        } else if (tn == "float") {
            emit(indent, "buf.write(struct.pack('>f', %s))", accessor);
        } else if (tn == "double") {
            emit(indent, "buf.write(struct.pack('>d', %s))", accessor);
        } else {
            auto& sn = zm.type.shortname;
            auto* gpf = "_get_packed_fingerprint()";
            if (zm.type.fullname == zs.structname.fullname) {
                emit(indent, "assert %s.%s == %s.%s", accessor, gpf, sn.c_str(), gpf);
            } else {
                emit(indent, "assert %s.%s == %s.%s",
                     accessor, gpf, zm.type.nameUnderscoreCStr(), gpf);
            }
            emit(indent, "%s._encode_one(buf)", accessor);
        }
    }

    void emitEncodeList(const ZCMMember& zm, const string& accessor_, int indent,
                        const string& len_, int fixedLen)
    {
        auto& tn = zm.type.fullname;
        auto* accessor = accessor_.c_str();
        auto* len = len_.c_str();

        if (zm.type.numbits != 0) {
            string f = getStructFormat(zm);
            f[0] = 'u'; // Encoding is all unsigned
            emit(indent, "numbits = %s%s * %u", fixedLen ? "" : "self.", len, zm.type.numbits);
            emit(indent, "mask = (1 << numbits) - 1");
            emit(indent, "numbits += offset_bit");
            emit(indent, "bitbuf = bytearray(math.ceil(numbits / 8))");
            emit(indent, "if (offset_bit != 0):");
            emit(indent + 1, "buf.seek(-1, os.SEEK_CUR)");
            emit(indent + 1, "bitbuf[0] = buf.read(1)[0]");
            emit(indent + 1, "buf.seek(-1, os.SEEK_CUR)");
            emit(indent, "formatstr = %s%s * \"%s\"", fixedLen ? "" : "self.", len, f.c_str());
            emit(indent, "bitstruct.pack_into('>' + formatstr + '>', "
                         "bitbuf, offset_bit, *(f & mask for f in %s[:%s%s]))",
                         accessor, fixedLen ? "" : "self.", len);
            emit(indent, "buf.write(bitbuf)");
        } else if (tn == "byte") {
            emit(indent, "buf.write(bytearray(%s[:%s%s]))",
                 accessor, (fixedLen ? "" : "self."), len);
            return;
        } else if (tn == "boolean" || tn == "int8_t" || tn == "int16_t" || tn == "int32_t" ||
                   tn == "int64_t" || tn == "float"  || tn == "double") {
            if (fixedLen) {
                emit(indent, "buf.write(struct.pack('>%s%s', *%s[:%s]))",
                     len, getStructFormat(zm).c_str(), accessor, len);
            } else {
                emit(indent, "buf.write(struct.pack('>%%d%s' %% self.%s, *%s[:self.%s]))",
                     getStructFormat(zm).c_str(), len, accessor, len);
            }
        } else {
            assert(0);
        }
    }

    void flushWriteStructFmt(vector<string>& formats, vector<const ZCMMember*>& members)
    {
        size_t nfmts = formats.size();
        assert(nfmts == members.size());
        if (nfmts == 0)
            return;

        if (members[0]->type.numbits != 0) {
            size_t numbits = 0;
            for (const auto* zm : members) numbits += zm->type.numbits;
            emit(2, "numbits = offset_bit + %u", numbits);
            emit(2, "bitbuf = bytearray(math.ceil(numbits / 8))");
            emit(2, "if (offset_bit != 0):");
            emit(3, "buf.seek(-1, os.SEEK_CUR)");
            emit(3, "bitbuf[0] = buf.read(1)[0]");
            emit(3, "buf.seek(-1, os.SEEK_CUR)");
            emitStart(2, "bitstruct.pack_into(\">");
            for (string f : formats) {
                f[0] = 'u'; // make unsigned for encoding
                emitContinue("%s", f.c_str());
            }
            emitContinue ("\", bitbuf, offset_bit, ");
            for (size_t i = 0; i < members.size(); ++i) {
                auto* zm = members[i];
                emitContinue("self.%s & ((1 << %u) - 1)",
                             zm->membername.c_str(), zm->type.numbits);
                if (i != members.size() - 1) emitContinue(", ");
            }
            emitEnd(")");
            emit(2, "buf.write(bitbuf)");
        } else {
            emitStart(2, "buf.write(struct.pack(\">");
            for (const auto& f : formats) emitContinue("%s", f.c_str());
            emitContinue ("\", ");
            for (size_t i = 0; i < members.size(); ++i) {
                auto* zm = members[i];
                emitContinue("self.%s", zm->membername.c_str());
                if (i != members.size() - 1) emitContinue(", ");
            }
            emitEnd("))");
        }

        formats.clear();
        members.clear();
    }

    void emitPythonEncodeOne()
    {
        emit(1, "def _encode_one(self, buf):");
        if (zs.members.size() == 0) {
            emit(2, "pass");
            return;
        }

        vector<string> structFmt;
        vector<const ZCMMember*> structMembers;

        bool inBitMode = false;
        size_t bitfieldNum = 0;
        for (auto& zm : zs.members) {
            string fmt = getStructFormat(zm);
            bool isolate = fmt.empty() || zm.type.fullname == "boolean";

            if (!inBitMode && zm.type.numbits != 0) {
                if (!structFmt.empty()) {
                    flushWriteStructFmt(structFmt, structMembers);
                }
                inBitMode = true;
                emit(0, "");
                emit(2, "# Start of bitfield %u", bitfieldNum);
                emit(0, "");
                emit(2, "offset_bit = 0;");
                emit(0, "");
            } else if (inBitMode && zm.type.numbits == 0) {
                if (!structFmt.empty()) {
                    flushWriteStructFmt(structFmt, structMembers);
                }
                inBitMode = false;
                emit(0, "");
                emit(2, "# End of bitfield %u", bitfieldNum);
                emit(0, "");
                ++bitfieldNum;
            }

            if (zm.dimensions.size() == 0) {
                if (isolate) {
                    emitEncodeOne(zm, "self." + zm.membername, 2);
                } else {
                    structFmt.push_back(fmt);
                    structMembers.push_back(&zm);
                }
            } else {
                if (!structFmt.empty()) {
                    flushWriteStructFmt(structFmt, structMembers);
                    if (inBitMode) {
                        emit(2, "offset_bit = numbits %% 8");
                        emit(0, "");
                    }
                }

                string accessor = "self." + zm.membername;
                size_t n = 0;
                for (; n < zm.dimensions.size()-1; ++n) {
                    auto& dim = zm.dimensions[n];
                    accessor += "[i" + to_string(n) + "]";
                    if (dim.mode == ZCM_CONST) {
                        emit(2+n, "for i%d in range(%s):", n, dim.size.c_str());
                    } else {
                        emit(2+n, "for i%d in range(self.%s):", n, dim.size.c_str());
                    }
                }

                // last dimension.
                auto& lastDim = zm.dimensions[zm.dimensions.size()-1];
                bool lastDimFixedLen = (lastDim.mode == ZCM_CONST);

                if (ZCMGen::isPrimitiveType(zm.type.fullname) &&
                    zm.type.fullname != "string") {
                    emitEncodeList(zm, accessor, 2+n, lastDim.size, lastDimFixedLen);
                    if (inBitMode) {
                        emit(2+n, "offset_bit = numbits %% 8");
                        emit(0, "");
                    }
                } else {
                    if (lastDimFixedLen) {
                        emit(2+n, "for i%d in range(%s):", n, lastDim.size.c_str());
                    } else {
                        emit(2+n, "for i%d in range(self.%s):", n, lastDim.size.c_str());
                    }
                    accessor += "[i" + to_string(n) + "]";
                    emitEncodeOne(zm, accessor, n+3);
                    if (inBitMode) {
                        emit(2+n, "offset_bit = numbits %% 8");
                        emit(0, "");
                    }
                }
            }
        }
        flushWriteStructFmt(structFmt, structMembers);
        if (inBitMode) {
            emit(2, "# End of bitfield %u", bitfieldNum);
        }
        emit(0, "");
    }

    void emitPythonEncode()
    {
        emit(1, "def encode(self):");
        emit(1, "    buf = BytesIO()");
        emit(1, "    buf.write(%s._get_packed_fingerprint())", zs.structname.shortname.c_str());
        emit(1, "    self._encode_one(buf)");
        emit(1, "    return buf.getvalue()");
        emit(0, "");
    }

    void emitMemberInitializer(const ZCMMember& zm, int dimNum)
    {
        if((size_t)dimNum == zm.dimensions.size()) {
            auto& tn = zm.type.fullname;
            const char* initializer = nullptr;
            if (tn == "byte") initializer = "0";
            else if (tn == "boolean") initializer = "False";
            else if (tn == "int8_t")  initializer = "0";
            else if (tn == "int16_t") initializer = "0";
            else if (tn == "int32_t") initializer = "0";
            else if (tn == "int64_t") initializer = "0";
            else if (tn == "float")   initializer = "0.0";
            else if (tn == "double")  initializer = "0.0";
            else if (tn == "string")  initializer = "\"\"";

            if (initializer) {
                emitContinue("%s", initializer);
            } else {
                emitContinue("%s()", zm.type.nameUnderscoreCStr());
            }
            return;
        }
        // Arrays of bytes get treated as strings, so that they can be more
        // efficiently packed and unpacked.
        if ((size_t)dimNum == zm.dimensions.size() - 1 &&
            zm.type.fullname == "byte") {
            emitContinue("bytearray()");
            return;
        }
        auto& dim = zm.dimensions[dimNum];
        if (dim.mode == ZCM_VAR) {
            emitContinue("[]");
        } else {
            emitContinue("[ ");
            emitMemberInitializer(zm, dimNum+1);
            emitContinue(" for dim%d in range(%s) ]", dimNum, dim.size.c_str());
        }
    }

    void emitPythonInit()
    {
        emit(1,"def __init__(self):");
        size_t i = 0;
        for (; i < zs.members.size(); ++i) {
            auto& zm = zs.members[i];
            emitStart(1, "    self.%s = ", zm.membername.c_str());
            emitMemberInitializer(zm, 0);
            emitEnd("");
        }
        if (i == 0)
            emit(1,"    pass");
        emit(0, "");
    }

    void emitPythonFingerprint()
    {
        auto& sn_ = zs.structname.shortname;
        auto* sn = sn_.c_str();

        emit(1, "_hash = None");

        emit(1, "def _get_hash_recursive(parents):");
        emit(2,     "if %s in parents: return 0", sn);
        for (auto& zm : zs.members) {
            if (!ZCMGen::isPrimitiveType(zm.type.fullname)) {
                emit(2,     "newparents = parents + [%s]", sn);
                break;
            }
        }
        emitStart(2, "tmphash = (0x%" PRIx64, zs.hash);
        for (auto &zm : zs.members) {
            auto& msn = zm.type.shortname;
            if (!ZCMGen::isPrimitiveType(zm.type.fullname)) {
                const char* ghr = "_get_hash_recursive(newparents)";
                if (zm.type.fullname == zs.structname.fullname) {
                    emitContinue("+ %s.%s", msn.c_str(), ghr);
                } else {
                    emitContinue("+ %s.%s", zm.type.nameUnderscoreCStr(), ghr);
                }
            }
        }
        emitEnd (") & 0xffffffffffffffff");

        emit(2, "tmphash  = (((tmphash<<1)&0xffffffffffffffff)  + "
             "((tmphash>>63)&0x1)) & 0xffffffffffffffff");
        emit (2,     "return tmphash");
        emit (1, "_get_hash_recursive = staticmethod(_get_hash_recursive)");

        emit (1, "_packed_fingerprint = None");
        emit (0, "");
        emit (1, "def _get_packed_fingerprint():");
        emit (2,     "if %s._packed_fingerprint is None:", sn);
        emit (3,         "%s._packed_fingerprint = struct.pack(\">Q\", "
              "%s._get_hash_recursive([]))", sn, sn);
        emit (2,     "return %s._packed_fingerprint", sn);
        emit (1, "_get_packed_fingerprint = staticmethod(_get_packed_fingerprint)");
        emit(0, "");

    }

    void emitPythonDependencies()
    {
        unordered_map<string, const ZCMTypename&> dependencies;
        for (auto& zm : zs.members) {
            auto& tn = zm.type.fullname;
            if (!ZCMGen::isPrimitiveType(tn) &&
                dependencies.find(tn) == dependencies.end() &&
                tn != zs.structname.fullname) {
                dependencies.insert({tn, zm.type});
            }
        }

        for (auto& p : dependencies) {
            auto* tn = p.first.c_str();
            auto& type = p.second;
            if (type.package.empty()) {
                emit(0, "from %s import %s", tn, tn);
            } else {
                emit(0, "from %s import %s as %s",
                        tn, type.shortname.c_str(), type.nameUnderscoreCStr());
            }
            emit(0,"");
        }
    }
};

// XXX: this file should really be updated to work more like the julia version
//      currently, this can technically run into parallelization problems because
//      multiple threads could be running zcmgen to access the same package.
//      There is probably some happy medium between how julia is currently doing
//      it (requiring single call, every type access for generating the package
//      files) and the way python is doing it (reading in and modifying the
//      python package files) by using filesystem locks, but that is beyond the
//      scope of this branch.
struct PyEmitPack
{
    const ZCMGen& zcm;

    PyEmitPack(const ZCMGen& zcm):
        zcm(zcm) {}

    int emitPackage(const string& packName, vector<const ZCMStruct*>& packStructs)
    {
        // create the package directory, if necessary
        vector<string> dirs = StringUtil::split(packName, '.');
        string pdname = StringUtil::join(dirs, '/');
        int havePackage = dirs.size() > 0;

        auto& ppath = zcm.gopt->getString("ppath");
        string packageDirPrefix = ppath + ((ppath.size() > 0) ? "/" : "");
        string packageDir = packageDirPrefix + pdname + (havePackage ? "/" : "");

        if (packageDir != "") {
            if (!FileUtil::exists(packageDir)) {
                FileUtil::mkdirWithParents(packageDir, 0755);
            }
            if (!FileUtil::dirExists(packageDir)) {
                cerr << "Could not create directory " << packageDir << "\n";
                return -1;
            }
        }

        // write the package __init__.py files, if necessary
        FILE* initPyFp = nullptr;
        unordered_set<string> initPyImports;

        if (havePackage) {
            size_t ndirs = dirs.size();
            for (size_t i = 0; i < ndirs; ++i) {

                vector<string> initPyFnameParts;
                initPyFnameParts.push_back(packageDirPrefix);
                for (size_t j = 0; j <= i; ++j)
                    initPyFnameParts.push_back(dirs[j]);
                initPyFnameParts.push_back("__init__.py");

                string initpyFname = StringUtil::join(initPyFnameParts, '/');
                bool createdInitpy = false;

                // close init_py_fp if already open
                if (initPyFp) {
                    fclose(initPyFp);
                    initPyFp = nullptr;
                }

                if (!FileUtil::exists(initpyFname)) {
                    // __init__.py does not exist for this package.  Create it.
                    createdInitpy = true;
                    initPyFp = fopen(initpyFname.c_str(), "w");
                } else {
                    // open the existing __init__.py file, and make note of the
                    // modules it imports
                    createdInitpy = 0;
                    initPyFp = fopen(initpyFname.c_str(), "r+");
                }

                if (!initPyFp) {
                    perror("fopen");
                    return -1;
                }

                //////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////

                if(createdInitpy) {
                    fprintf(initPyFp, "\"\"\"ZCM package __init__.py file\n"
                             "This file automatically generated by zcm-gen.\n"
                             "DO NOT MODIFY BY HAND!!!!\n"
                             "\"\"\"\n\n");
                } else {
                    while(!feof(initPyFp)) {
                        char buf[4096];
                        memset(buf, 0, sizeof(buf));
                        char* result = fgets(buf, sizeof(buf)-1, initPyFp);
                        if (!result)
                            break;
                        auto words = StringUtil::split(StringUtil::strip(buf), ' ');
                        if (words.size() < 4)
                            continue;
                        if (words[0] == "from" && words[2] == "import") {
                            string module = string(words[1].c_str()+1);
                            initPyImports.insert(std::move(module));
                        }
                    }
                }
            }
        }

        ////////////////////////////////////////////////////////////
        // STRUCTS
        for (auto* zs_ : packStructs) {
            auto& zs = *zs_;
            auto& sn_ = zs.structname.shortname;
            auto* sn = sn_.c_str();
            string path = packageDir + sn_ + ".py";

            if(initPyFp && initPyImports.find(sn_) == initPyImports.end()) {
                fprintf(initPyFp, "from .%s import %s\n", sn, sn);
            }

            if (!zcm.needsGeneration(zs.zcmfile, path))
                continue;

            PyEmitStruct{zcm, zs, path}.emitStruct();
        }

        if(initPyFp)
            fclose(initPyFp);

        return 0;
    }
};

int emitPython(const ZCMGen& zcm)
{
    if (zcm.gopt->getBool("little-endian-encoding")) {
        printf("Python does not currently support little endian encoding\n");
        return -1;
    }

    unordered_map<string, vector<const ZCMStruct*> > packages;

    // group the structs by package
    for (auto& zs : zcm.structs)
        packages[zs.structname.package].push_back(&zs);

    for (auto& kv : packages) {
        auto& name = kv.first;
        auto& pack = kv.second;

        int ret = PyEmitPack{zcm}.emitPackage(name, pack);
        if (ret != 0) return ret;
    }

    return 0;
}

vector<string> getFilepathsPython(const ZCMGen& zcm)
{
    vector<string> ret;

    unordered_map<string, vector<const ZCMStruct*> > packages;

    // group the structs by package
    for (auto& zs : zcm.structs)
        packages[zs.structname.package].push_back(&zs);

    for (auto& kv : packages) {
        auto& name = kv.first;
        auto& pack = kv.second;

        // TODO: definitely should push this into a static funciton once we clean up this file
        vector<string> dirs = StringUtil::split(name, '.');
        string pdname = StringUtil::join(dirs, '/');
        int havePackage = dirs.size() > 0;

        auto& ppath = zcm.gopt->getString("ppath");
        string packageDirPrefix = ppath + ((ppath.size() > 0) ? "/" : "");
        string packageDir = packageDirPrefix + pdname + (havePackage ? "/" : "");

        for (auto* zs : pack) {
            string path = packageDir + zs->structname.shortname + ".py";
            ret.push_back(path);
        }
    }

    return ret;
}

unordered_set<string> getReservedKeywordsPython()
{
    return { "False", "class", "finally", "is", "return", "None", "continue",
             "for", "lambda", "try", "True", "def", "from", "nonlocal",
             "while", "and", "del", "global", "not", "with", "as", "elif",
             "if", "or", "yield", "assert", "else", "import", "pass", "break",
             "except", "in", "raise" };
}
