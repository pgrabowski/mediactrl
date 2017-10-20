Name:       mediactrl
Version:    0.4
Release:    1
Summary:    Mediactrl server.
Group:      Development/Tools
License:    GNU GPL 2.0
URL:        https://github.com/pgrabowski/mediactrl
Source0:    https://github.com/pgrabowski/mediactrl/archive/master.zip
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
BuildRequires: gcc-c++ libtool make automake c-ares-devel ffmpeg-devel ortp-devel resiprocate-devel expat-devel boost-devel gsm-devel libcurl-devel openssl-devel commoncpp2-devel
BuildArch: x86_64
Requires(post): /sbin/install-info
Requires(postun):   /sbin/install-info
Requires: ffmpeg ortp resiprocate expat boost gsm libcurl openssl commoncpp2

%description
Mediactrl server.

%prep
%setup -q -n mediactrl-master

%build
autoreconf -is
%configure --prefix=/usr
make

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}
rm -rf %{buildroot}%{_infodir}/dir

%clean
rm -rf %{buildroot}

%post
/sbin/install-info %{_infodir}/autoconf.info %{_infodir}/dir || :

%preun
if [ "$1" = 0 ]; then
/sbin/install-info --del %{_infodir}/autoconf.info %{_infodir}/dir || :
fi

%files
%defattr(-,root,root,-)
%doc AUTHORS COPYING ChangeLog NEWS README THANKS TODO
%exclude %{_infodir}/standards*
%{_bindir}/*
%{_datadir}/autoconf
%{_infodir}/*
%{_mandir}/*/*

%changelog
* Fri Oct 20 2017 Piotr Grabowski <github.com/pgrabowski> 0.4
- Created Initial Spec File.
