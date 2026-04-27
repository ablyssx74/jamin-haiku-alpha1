name			$(NAME)
version			$(VERSION)-1
architecture	$(ARCH)
summary 		"JAMin Audio Mastering interface."
description 	"JAMin is an open source application designed to perform professional audio mastering of stereo input streams. It uses LADSPA for digital signal processing (DSP)."
packager		"ablyss <jb@epluribusunix.net>"
vendor			"$(NAME) Project"
licenses {
	"GNU GPL v2"
}
copyrights {
	"$(YEAR) $(NAME) project"
}
provides {
	$(NAME) = $(VERSION)-1
}
requires {
	haiku
    fftw
    gtk3_x86
    libxml2_x86
    glib2_x86
}	
urls {
	"https://github.com/ablyssx74/jamin-haiku"
}

