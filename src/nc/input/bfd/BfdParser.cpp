/* The file is part of Snowman decompiler. */
/* See doc/licenses.asciidoc for the licensing information. */

#include "BfdParser.h"

#include <QCoreApplication> /* For Q_DECLARE_TR_FUNCTIONS. */
#include <QFile>
#include <QDebug>

#include <nc/common/Foreach.h>
#include <nc/common/LogToken.h>
#include <nc/common/Range.h>
#include <nc/common/make_unique.h>

#include <nc/core/image/Image.h>
#include <nc/core/image/Reader.h>
#include <nc/core/image/Relocation.h>
#include <nc/core/image/Section.h>
#include <nc/core/input/ParseError.h>
#include <nc/core/input/Utils.h>

#define PACKAGE 1 /* Work-around for bfd.h */
#define PACKAGE_VERSION 1 /* Work-around for bfd.h */

#include "bfd.h"

namespace nc {
namespace input {
namespace bfdparser {

namespace {

using nc::core::input::read;
using nc::core::input::getAsciizString;
using nc::core::input::ParseError;

class BfdParserImpl {
    Q_DECLARE_TR_FUNCTIONS(BfdParserImpl)

    QIODevice *source_;
    core::image::Image *image_;
    const LogToken &log_;
    bfd *abfd = nullptr;

    ByteOrder byteOrder_;
    std::vector<const core::image::Section *> sections_;
    std::vector<const core::image::Symbol *> symbols_;

public:
    BfdParserImpl(QIODevice *source, core::image::Image *image, const LogToken &log):
        source_(source), image_(image), log_(log), byteOrder_(ByteOrder::Current)
    {}

    void parse() {
		QFile *file = static_cast<QFile *>(source_);
		QString filename = file->fileName();
		source_->seek(0); /* Convention */

		/* Read filename */
		if((abfd = bfd_openr(filename.toAscii().data(), nullptr)) == nullptr){
			throw ParseError(tr("Could not open file: %1").arg(filename));
		}
  
  		/* Here we try to figure out the target. */
  		if (!bfd_check_format(abfd, bfd_object)){
			bfd_close(abfd);
			throw ParseError(tr("Could not open file: %1").arg(filename));
		}
  
  		int arch = bfd_get_arch(abfd);
  		byteOrder_ = bfd_big_endian(abfd) ? ByteOrder::BigEndian : ByteOrder::LittleEndian;
  	
  		switch (arch) {
            case bfd_arch_i386:
                if(bfd_get_arch_size(abfd) == 32){
	                image_->platform().setArchitecture(QLatin1String("i386"));
                } else {
                	image_->platform().setArchitecture(QLatin1String("x86-64"));
                }
                break;
            case bfd_arch_arm:
                if (byteOrder_ == ByteOrder::LittleEndian) {
                    image_->platform().setArchitecture(QLatin1String("arm-le"));
                } else {
                    image_->platform().setArchitecture(QLatin1String("arm-be"));
                }
                break;
            case bfd_arch_mips:
                if (byteOrder_ == ByteOrder::LittleEndian) {
					if(bfd_get_arch_size(abfd) == 32) {
                        image_->platform().setArchitecture(QLatin1String("mips-le"));
                    } else {
                    	image_->platform().setArchitecture(QLatin1String("mips64-le"));
                    }
                } 
                else if(bfd_get_arch_size(abfd) == 32) {
                    image_->platform().setArchitecture(QLatin1String("mips-be"));
                } else {
                	image_->platform().setArchitecture(QLatin1String("mips64-be"));
                }
                break;
            default:
            	const char *id = bfd_printable_name(abfd);
            	bfd_close(abfd);
                throw ParseError(tr("Unknown machine id: %1.").arg(getAsciizString(id)));
        }

        parseSections();
        parseSymbols();
        parseRelocations();     

		bfd_close(abfd);
		return;        
    }

private:

    void parseSections() {
		return;
    }

    void parseSymbols() {
		long storage;
		long symcount;
		asymbol **syms;
		bfd_boolean dynamic = FALSE;
		
		if (!(bfd_get_file_flags(abfd) & HAS_SYMS))
    		return;

  		storage = bfd_get_symtab_upper_bound(abfd);
  		if (storage == 0) {
  			storage = bfd_get_dynamic_symtab_upper_bound(abfd);
  			dynamic = TRUE;
  		}
  		
  		if (storage < 0) {
  			bfd_close(abfd);
  			throw ParseError(tr("bfd_get_symtab_upper_bound: %1.").arg(getAsciizString(bfd_errmsg(bfd_get_error()))));
	  	}

 		syms = (asymbol **)malloc(storage);
 		if (dynamic){
 			symcount = bfd_canonicalize_dynamic_symtab(abfd, syms);
    	} else {
 			symcount = bfd_canonicalize_symtab(abfd, syms);
    	}
    	
		if (symcount < 0) {
			bfd_close(abfd);
			free(syms);
			syms = nullptr;
			throw ParseError(tr("bfd_canonicalize_symtab: %1.").arg(getAsciizString(bfd_errmsg(bfd_get_error()))));
		}

		for (int i = 0; i < symcount; i++) {
			using core::image::Symbol;
			using core::image::SymbolType;
			asymbol *asym = syms[i];
			const char *sym_name = bfd_asymbol_name(asym);
			char symclass = bfd_decode_symclass(asym);
			int sym_value = bfd_asymbol_value(asym);

			QString name = getAsciizString(sym_name);
			boost::optional<ConstantValue> value = static_cast<long>(sym_value);
			const core::image::Section *section = nullptr;

			/* qDebug()  << name << " is: "  << symclass; */
			
            SymbolType type;
			switch (symclass) {
				case 'A':
				case 'a':
				case 'D':
				case 'd':
				case 'G':
				case 'g':
				case 'S': /* Small object */
				case 's':
					type = SymbolType::OBJECT;
					break;
				case 'I':
				case 'i':
				case 'T': /* .text */
				case 't':
				case 'U':
				case 'u':
				case 'V':
				case 'v':
				case 'W':
				case 'w':
					type = SymbolType::FUNCTION;
					break;
				case 'B': /* BSS */
				case 'b':
				case 'R': /* Read-only */
				case 'r':
				case 'N': /* Debug */
				case 'n':
					type = SymbolType::SECTION;
					break;
				default:
					type = SymbolType::NOTYPE;
					break;
			}

            auto sym = std::make_unique<core::image::Symbol>(type, name, value, section);
            symbols_.push_back(sym.get());
            image_->addSymbol(std::move(sym));
		}

    	free(syms);
		syms = nullptr;
		return;
    }

    void parseRelocations() {
		return;
    }

};

} // anonymous namespace

BfdParser::BfdParser():
    core::input::Parser(QLatin1String("BFD"))
{}

bool BfdParser::doCanParse(QIODevice *source) const {
	bfd *abfd = nullptr;
	QFile *file = static_cast<QFile *>(source);
	QString filename = file->fileName();
	
	bfd_init();
	abfd = bfd_openr(filename.toAscii().data(), nullptr);
	if(abfd == nullptr){
		return false;
	}
	if (!bfd_check_format(abfd, bfd_object)){
		bfd_close(abfd);
		return false;
	}
	bfd_close(abfd);
	return true;
}

void BfdParser::doParse(QIODevice *source, core::image::Image *image, const LogToken &log) const {
	BfdParserImpl(source, image, log).parse();
}

} // namespace bfdparser
} // namespace input
} // namespace nc

/* vim:set et sts=4 sw=4: */
