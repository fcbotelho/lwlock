%define name lwlock
%define version 1.0
%define release 1

Name: %{name}
Version: %{version}
Release: %{release}
Summary: Light-Weight Lock Library
Source: %{name}-%{version}.tar.gz
License: Open Source
URL: http://lwlock.sf.net
BuildArch: i386
Group: Library
BuildRoot: %{_tmppath}/%{name}-root

%description
Light-Weight Lock Library

%prep
rm -Rf $RPM_BUILD_ROOT
rm -rf $RPM_BUILD_ROOT
%setup
mkdir $RPM_BUILD_ROOT
mkdir $RPM_BUILD_ROOT/usr
CXXFLAGS="-O2" ./configure --prefix=/usr/

%build
make

%install
DESTDIR=$RPM_BUILD_ROOT make install

%files
%defattr(755,root,root)
/

%changelog
* Fri Jun 21 2013 Fabiano C. Botelho <fabiano.botelho@emc.com>
+ Initial build
