/*
 * Rufus for macOS: Windows User Experience (unattend.xml) customization
 * Copyright © 2022-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus macOS port contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The XML generation is a port of CreateUnattendXml() from src/wue.c.
 *
 * Where Windows Rufus edits the offline registry inside boot.wim (which relies
 * on Windows' registry APIs), the macOS version places the answer file at the
 * root of the media as autounattend.xml, which Windows Setup applies to all
 * passes, including windowsPE. Otherwise, as on Windows, the file goes to
 * \sources\$OEM$\$$\Panther\unattend.xml.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include <cdio/cdio.h>
#include <cdio/iso9660.h>

#include "rufus_core.h"

static const char* bypass_name[] = { "BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck" };
#define USERNAME_INVALID_CHARS "/\\[]:;|=,+*?<>\"%"

typedef struct {
	char* s[64];
	int n;
} cmd_list_t;

static void cmd_add(cmd_list_t* l, const char* c)
{
	if (l->n < 64)
		l->s[l->n++] = strdup(c);
}

static void cmd_clear(cmd_list_t* l)
{
	for (int i = 0; i < l->n; i++)
		free(l->s[i]);
	l->n = 0;
}

static const char* xml_arch(const image_report_t* r)
{
	/* has_efi bit (2 << i), with i the index in efi_archname[] (see iso.c) */
	if (r->has_efi & (2 << 4))
		return "arm64";
	if (r->has_efi & (2 << 3))
		return "arm";
	if ((r->has_efi & (2 << 1)) && !(r->has_efi & (2 << 2)))
		return "x86";
	return "amd64";
}

#define COMPONENT(name) \
	fprintf(fd, "    <component name=\"" name "\" processorArchitecture=\"%s\" language=\"neutral\" " \
		"xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" " \
		"publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\n", arch)

char* create_unattend_xml(const image_report_t* r, const wue_options_t* w)
{
	static const char* unallowed_account_names[] = {
		"Administrator", "Järjestelmänvalvoja", "Administrateur", "Rendszergazda", "Administrador", "Администратор", "Administratör",
		"Guest", "DefaultAccount", "WDAGUtilityAccount", "HelpAssistant", "KRBTGT", "Local", "NONE", "SYSTEM"
	};
	const char* arch = xml_arch(r);
	const int flags = w->flags;
	cmd_list_t commands = { 0 };
	char* buf = NULL;
	size_t len = 0;
	char tmp[256], username[64];
	FILE* fd;
	int i, order;

	if (flags == 0) {
		uprintf("Note: No Windows User Experience options selected");
		return NULL;
	}
	fd = open_memstream(&buf, &len);
	if (fd == NULL)
		return NULL;
	uprintf("Selected Windows User Experience options:");
	fprintf(fd, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	fprintf(fd, "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");

	if (flags & UNATTEND_WINPE_SETUP_MASK) {
		fprintf(fd, "  <settings pass=\"windowsPE\">\n");
		COMPONENT("Microsoft-Windows-Setup");
		fprintf(fd, "      <UserData>\n");
		fprintf(fd, "        <AcceptEula>true</AcceptEula>\n");
		fprintf(fd, "        <ProductKey>\n");
		fprintf(fd, "          <Key />\n");
		fprintf(fd, "        </ProductKey>\n");
		fprintf(fd, "      </UserData>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			uprintf("• ⚠Silent Install⚠");
			fprintf(fd, "      <DiskConfiguration>\n");
			fprintf(fd, "        <WillShowUI>OnError</WillShowUI>\n");
			if (flags & UNATTEND_DISABLE_BITLOCKER)
				fprintf(fd, "        <DisableEncryptedDiskProvisioning>true</DisableEncryptedDiskProvisioning>\n");
			fprintf(fd, "        <Disk wcm:action=\"modify\">\n");
			fprintf(fd, "          <DiskID>1</DiskID> \n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"modify\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>2</PartitionID>\n");
			fprintf(fd, "              <Label>RUFUS_BOOT</Label>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "        <Disk wcm:action=\"add\">\n");
			fprintf(fd, "          <DiskID>0</DiskID> \n");
			fprintf(fd, "          <WillWipeDisk>true</WillWipeDisk> \n");
			fprintf(fd, "          <CreatePartitions>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <Type>EFI</Type>\n");
			fprintf(fd, "              <Size>260</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <Type>MSR</Type>\n");
			fprintf(fd, "              <Size>16</Size>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "            <CreatePartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>3</Order>\n");
			fprintf(fd, "              <Type>Primary</Type>\n");
			fprintf(fd, "              <Extend>true</Extend>\n");
			fprintf(fd, "            </CreatePartition>\n");
			fprintf(fd, "          </CreatePartitions>\n");
			fprintf(fd, "          <ModifyPartitions>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>1</Order>\n");
			fprintf(fd, "              <PartitionID>1</PartitionID>\n");
			fprintf(fd, "              <Label>EFI</Label>\n");
			fprintf(fd, "              <Format>FAT32</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "            <ModifyPartition wcm:action=\"add\">\n");
			fprintf(fd, "              <Order>2</Order>\n");
			fprintf(fd, "              <PartitionID>3</PartitionID>\n");
			fprintf(fd, "              <Label>Windows</Label>\n");
			fprintf(fd, "              <Letter>C</Letter>\n");
			fprintf(fd, "              <Format>NTFS</Format>\n");
			fprintf(fd, "            </ModifyPartition>\n");
			fprintf(fd, "          </ModifyPartitions>\n");
			fprintf(fd, "        </Disk>\n");
			fprintf(fd, "      </DiskConfiguration>\n");
			fprintf(fd, "      <ImageInstall>\n");
			fprintf(fd, "        <OSImage>\n");
			fprintf(fd, "          <WillShowUI>OnError</WillShowUI>\n");
			fprintf(fd, "          <InstallFrom>\n");
			fprintf(fd, "            <MetaData wcm:action=\"add\">\n");
			fprintf(fd, "              <Key>/IMAGE/INDEX</Key>\n");
			fprintf(fd, "              <Value>%d</Value>\n", w->edition_index > 0 ? w->edition_index : 1);
			fprintf(fd, "            </MetaData>\n");
			fprintf(fd, "          </InstallFrom>\n");
			fprintf(fd, "          <InstallTo>\n");
			fprintf(fd, "            <DiskID>0</DiskID>\n");
			fprintf(fd, "            <PartitionID>3</PartitionID>\n");
			fprintf(fd, "          </InstallTo>\n");
			fprintf(fd, "        </OSImage>\n");
			fprintf(fd, "      </ImageInstall>\n");
		}
		if (flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
			uprintf("• Bypass SB/TPM/RAM");
			order = 1;
			fprintf(fd, "      <RunSynchronous>\n");
			for (i = 0; i < (int)(sizeof(bypass_name) / sizeof(bypass_name[0])); i++) {
				fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order++);
				fprintf(fd, "          <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n", bypass_name[i]);
				fprintf(fd, "        </RunSynchronousCommand>\n");
			}
			fprintf(fd, "      </RunSynchronous>\n");
		}
		fprintf(fd, "    </component>\n");
		if (flags & UNATTEND_SILENT_INSTALL) {
			COMPONENT("Microsoft-Windows-International-Core-WinPE");
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", w->locale[0] ? w->locale : "en-US");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_SPECIALIZE_DEPLOYMENT_MASK) {
		fprintf(fd, "  <settings pass=\"specialize\">\n");
		COMPONENT("Microsoft-Windows-Deployment");
		if (flags & UNATTEND_NO_ONLINE_ACCOUNT) {
			cmd_add(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\OOBE\" /v BypassNRO /t REG_DWORD /d 1 /f");
			uprintf("• Bypass online account requirement");
		}
		if (flags & UNATTEND_QOL_ENHANCEMENTS) {
			uprintf("• QoL: Disable OneDrive and Outlook by default");
			cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\OneDrive\" /v DisableFileSyncNGSC /t REG_DWORD /d 1 /f");
			cmd_add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Remove-Item -Path $env:SystemRoot\\System32\\OneDriveSetup.exe -Force -Confirm:$false; "
				"Remove-Item -Path $env:SystemRoot\\SysWOW64\\OneDriveSetup.exe -Force -Confirm:$false;\"");
			cmd_add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Outlook*'} |"
				" Remove-AppxProvisionedPackage -Online\"");
			cmd_add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Outlook* | Remove-AppxPackage -AllUsers\"");
			cmd_add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxProvisionedPackage -Online | Where-Object {$_.PackageName -like '*Teams*'} |"
				" Remove-AppxProvisionedPackage -Online\"");
			cmd_add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
				"\"Get-AppxPackage -AllUsers *Teams* | Remove-AppxPackage -AllUsers\"");
		}
		for (order = 1; order <= commands.n; order++) {
			if (order == 1)
				fprintf(fd, "      <RunSynchronous>\n");
			fprintf(fd, "        <RunSynchronousCommand wcm:action=\"add\">\n");
			fprintf(fd, "          <Order>%d</Order>\n", order);
			fprintf(fd, "          <Path>%s</Path>\n", commands.s[order - 1]);
			fprintf(fd, "        </RunSynchronousCommand>\n");
			if (order == commands.n)
				fprintf(fd, "      </RunSynchronous>\n");
		}
		fprintf(fd, "    </component>\n");
		fprintf(fd, "  </settings>\n");
		cmd_clear(&commands);
	}

	if (flags & UNATTEND_OOBE_MASK) {
		fprintf(fd, "  <settings pass=\"oobeSystem\">\n");
		if (flags & UNATTEND_OOBE_SHELL_SETUP_MASK || flags & (UNATTEND_APPLY_SKUSIPOLICY | UNATTEND_QOL_ENHANCEMENTS)) {
			COMPONENT("Microsoft-Windows-Shell-Setup");
			if (flags & (UNATTEND_NO_DATA_COLLECTION | UNATTEND_SILENT_INSTALL)) {
				uprintf("• Disable data collection");
				fprintf(fd, "      <OOBE>\n");
				fprintf(fd, "        <HideEULAPage>true</HideEULAPage>\n");
				fprintf(fd, "        <ProtectYourPC>3</ProtectYourPC>\n");
				if (flags & UNATTEND_SILENT_INSTALL) {
					fprintf(fd, "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n");
					fprintf(fd, "        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n");
				}
				fprintf(fd, "      </OOBE>\n");
			}
			if ((flags & UNATTEND_DUPLICATE_LOCALE) && w->timezone[0] != 0)
				fprintf(fd, "      <TimeZone>%s</TimeZone>\n", w->timezone);
			if (flags & UNATTEND_SET_USER) {
				snprintf(username, sizeof(username), "%s", w->username);
				for (i = 0; i < (int)(sizeof(unallowed_account_names) / sizeof(unallowed_account_names[0])) &&
					strcasecmp(username, unallowed_account_names[i]) != 0; i++);
				if (i < (int)(sizeof(unallowed_account_names) / sizeof(unallowed_account_names[0]))) {
					uprintf("WARNING: '%s' is not allowed as local account name - Option ignored", username);
				} else if (username[0] != 0) {
					bool sanitized = false;
					for (char* c = username; *c; c++) {
						if (strchr(USERNAME_INVALID_CHARS, *c) != NULL) {
							*c = '_';
							sanitized = true;
						}
					}
					uprintf("• Use '%s' for local account name", username);
					if (sanitized)
						uprintf("WARNING: Local account name contained unallowed characters and has been sanitized");
					fprintf(fd, "      <UserAccounts>\n");
					fprintf(fd, "        <LocalAccounts>\n");
					fprintf(fd, "          <LocalAccount wcm:action=\"add\">\n");
					fprintf(fd, "            <Name>%s</Name>\n", username);
					fprintf(fd, "            <DisplayName>%s</DisplayName>\n", username);
					fprintf(fd, "            <Group>Administrators;Power Users</Group>\n");
					fprintf(fd, "            <Password>\n");
					fprintf(fd, "              <Value>UABhAHMAcwB3AG8AcgBkAA==</Value>\n");
					fprintf(fd, "              <PlainText>false</PlainText>\n");
					fprintf(fd, "            </Password>\n");
					fprintf(fd, "          </LocalAccount>\n");
					fprintf(fd, "        </LocalAccounts>\n");
					fprintf(fd, "      </UserAccounts>\n");
					snprintf(tmp, sizeof(tmp), "net user \"%s\" /logonpasswordchg:yes", username);
					cmd_add(&commands, tmp);
					cmd_add(&commands, "net accounts /maxpwage:unlimited");
				}
			}
			if (flags & UNATTEND_APPLY_SKUSIPOLICY) {
				uprintf("• Apply SkuSiPolicy.p7b");
				cmd_add(&commands, "cmd /c mountvol S: /S &amp;&amp; "
					"copy %WINDIR%\\system32\\SecureBootUpdates\\SkuSiPolicy.p7b S:\\EFI\\Microsoft\\Boot &amp;&amp; "
					"mountvol S: /D");
			}
			if (flags & UNATTEND_QOL_ENHANCEMENTS) {
				uprintf("• QoL: Disable Fast Startup, Copilot, Recommendations, News and Teams by default");
				cmd_add(&commands, "reg add \"HKLM\\System\\CurrentControlSet\\Control\\Session Manager\\Power\" "
					"/v HiberbootEnabled /t REG_DWORD /d 0 /f");
				cmd_add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v ShowCopilotButton /t REG_DWORD /d 0 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\WindowsCopilot\" "
					"/v TurnOffWindowsCopilot /t REG_DWORD /d 1 /f");
				cmd_add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarMode /t REG_DWORD /d 1 /f");
				cmd_add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v SearchboxTaskbarModeCache /t REG_DWORD /d 1 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableWindowsConsumerFeatures /t REG_DWORD /d 1 /f");
				cmd_add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager\" "
					"/v SystemPaneSuggestionsEnabled /t REG_DWORD /d 0 /f");
				cmd_add(&commands, "reg add \"HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Search\" "
					"/v BingSearchEnabled /t REG_DWORD /d 0 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Device Metadata\" "
					"/v PreventDeviceMetadataFromNetwork /t REG_DWORD /d 1 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Dsh\" "
					"/v AllowNewsAndInterests /t REG_DWORD /d 0 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\Windows Feeds\" "
					"/v EnableFeeds /t REG_DWORD /d 0 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Communications\" "
					"/v ConfigureChatAutoInstall /t REG_DWORD /d 0 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Windows\\CloudContent\" "
					"/v DisableCloudOptimizedContent /t REG_DWORD /d 1 /f");
				cmd_add(&commands, "reg add \"HKLM\\Software\\Policies\\Microsoft\\Edge\" "
					"/v HideFirstRunExperience /t REG_DWORD /d 1 /f");
				uprintf("• QoL: More pins for the Start Menu and enable useful shortcuts");
				cmd_add(&commands, "reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\" "
					"/v Start_Layout /t REG_DWORD /d 1 /f");
				cmd_add(&commands, "PowerShell -NonInteractive -WindowStyle Hidden -Command "
					"\"Set-ItemProperty -Path 'Registry::HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Start' "
					"-Name 'VisiblePlaces' -Value $([convert]::FromBase64String('ztU0LVr6Q0WC8iLm6vd3PC+zZ+PeiVVDv85h83sYqTe8JIo"
					"UDNaJQqCAbtm7okiCRIF1/g0IrkKL2jTtl7ZjlEqwvXRK+WhPi9ZDmAcdqLyGCHNSqlFDQp97J3ZYRlnU')) -Type 'Binary'\"");
				uprintf("• QoL: Restore classic context menu");
				cmd_add(&commands, "reg add \"HKCU\\Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}\\InprocServer32\" "
					"/ve /t REG_SZ /d \"\" /f");
			}
			for (order = 1; order <= commands.n; order++) {
				if (order == 1)
					fprintf(fd, "      <FirstLogonCommands>\n");
				fprintf(fd, "        <SynchronousCommand wcm:action=\"add\">\n");
				fprintf(fd, "          <Order>%d</Order>\n", order);
				fprintf(fd, "          <CommandLine>%s</CommandLine>\n", commands.s[order - 1]);
				fprintf(fd, "        </SynchronousCommand>\n");
				if (order == commands.n)
					fprintf(fd, "      </FirstLogonCommands>\n");
			}
			fprintf(fd, "    </component>\n");
			cmd_clear(&commands);
		}
		if (flags & UNATTEND_OOBE_INTERNATIONAL_MASK) {
			const char* loc = w->locale[0] ? w->locale : "en-US";
			uprintf("• Use the same regional options as this user's");
			COMPONENT("Microsoft-Windows-International-Core");
			fprintf(fd, "      <InputLocale>%s</InputLocale>\n", w->input_locale[0] ? w->input_locale : loc);
			fprintf(fd, "      <SystemLocale>%s</SystemLocale>\n", loc);
			fprintf(fd, "      <UserLocale>%s</UserLocale>\n", loc);
			fprintf(fd, "      <UILanguage>%s</UILanguage>\n", loc);
			fprintf(fd, "      <UILanguageFallback>en-US</UILanguageFallback>\n");
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_DISABLE_BITLOCKER) {
			uprintf("• Disable BitLocker");
			COMPONENT("Microsoft-Windows-SecureStartup-FilterDriver");
			fprintf(fd, "      <PreventDeviceEncryption>true</PreventDeviceEncryption>\n");
			fprintf(fd, "    </component>\n");
			COMPONENT("Microsoft-Windows-EnhancedStorage-Adm");
			fprintf(fd, "      <TCGSecurityActivationDisabled>1</TCGSecurityActivationDisabled>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}

	if (flags & UNATTEND_OFFLINE_SERVICING_MASK) {
		fprintf(fd, "  <settings pass=\"offlineServicing\">\n");
		if (flags & UNATTEND_OFFLINE_INTERNAL_DRIVES) {
			uprintf("• Set internal drives offline");
			COMPONENT("Microsoft-Windows-PartitionManager");
			fprintf(fd, "      <SanPolicy>4</SanPolicy>\n");
			fprintf(fd, "    </component>\n");
		}
		if (flags & UNATTEND_FORCE_S_MODE) {
			uprintf("• Enforce S Mode");
			COMPONENT("Microsoft-Windows-CodeIntegrity");
			fprintf(fd, "      <SkuPolicyRequired>1</SkuPolicyRequired>\n");
			fprintf(fd, "    </component>\n");
		}
		fprintf(fd, "  </settings>\n");
	}
	fprintf(fd, "</unattend>\n");
	fclose(fd);
	return buf;
}

/* ------------------------------------------------------------------------ */
/* Windows version detection: parse the XML metadata of install.wim/esd      */
/* ------------------------------------------------------------------------ */
static bool read_iso_file_range(const char* iso_path, const char* src, uint64_t offset, void* buf, size_t len)
{
	iso9660_t* iso;
	iso9660_stat_t* st = NULL;
	iso_extension_mask_t masks[2] = { ISO_EXTENSION_ALL, ISO_EXTENSION_NONE };
	bool r = false;
	int i, fd;

	for (i = 0; i < 2 && st == NULL; i++) {
		iso = iso9660_open_ext(iso_path, masks[i]);
		if (iso == NULL)
			continue;
		st = iso9660_ifs_stat_translate(iso, src);
		iso9660_close(iso);
	}
	if (st == NULL)
		return false;
	/* Files are stored contiguously, so read straight from the image */
	if (offset + len <= st->total_size && (fd = open(iso_path, O_RDONLY)) >= 0) {
		r = pread(fd, buf, len, (off_t)((uint64_t)st->lsn * ISO_BLOCKSIZE + offset)) == (ssize_t)len;
		close(fd);
	}
	iso9660_stat_free(st);
	return r;
}

static char* get_wim_xml(const char* iso_path, const image_report_t* r)
{
	uint8_t hdr[208];
	uint64_t xml_offset, xml_size;
	uint8_t* xml16 = NULL;
	char* xml = NULL;
	size_t i;

	if (!HAS_WININST(r) || !read_iso_file_range(iso_path, r->wininst_path[0], 0, hdr, sizeof(hdr)))
		return NULL;
	if (memcmp(hdr, "MSWIM\0\0\0", 8) != 0)
		return NULL;
	xml_size = (uint64_t)hdr[0x48] | ((uint64_t)hdr[0x49] << 8) | ((uint64_t)hdr[0x4a] << 16) |
		((uint64_t)hdr[0x4b] << 24) | ((uint64_t)hdr[0x4c] << 32) | ((uint64_t)hdr[0x4d] << 40) | ((uint64_t)hdr[0x4e] << 48);
	memcpy(&xml_offset, &hdr[0x50], 8);
	if (xml_size < 16 || xml_size > 4 * MB)
		return NULL;
	xml16 = malloc((size_t)xml_size);
	xml = calloc(1, (size_t)xml_size / 2 + 1);
	if (xml16 == NULL || xml == NULL || !read_iso_file_range(iso_path, r->wininst_path[0], xml_offset, xml16, (size_t)xml_size)) {
		free(xml16);
		free(xml);
		return NULL;
	}
	/* The metadata is UTF-16LE, and all we need is ASCII */
	for (i = 0; i < xml_size / 2; i++)
		xml[i] = (xml16[2 * i + 1] == 0) ? (char)xml16[2 * i] : '?';
	free(xml16);
	return xml;
}

int get_windows_editions(const char* iso_path, const image_report_t* r, char names[][128], int max)
{
	char* xml = get_wim_xml(iso_path, r);
	char* p = xml;
	int n = 0;
	while (p != NULL && n < max && (p = strstr(p, "<IMAGE INDEX=")) != NULL) {
		char* end = strstr(p, "</IMAGE>");
		char* dn = strstr(p, "<DISPLAYNAME>");
		if (dn == NULL || (end != NULL && dn > end))
			dn = strstr(p, "<NAME>");
		if (dn != NULL && (end == NULL || dn < end)) {
			char* s = strchr(dn, '>') + 1;
			char* e = strchr(s, '<');
			snprintf(names[n], 128, "%.*s", (int)(e - s), s);
		} else {
			snprintf(names[n], 128, "Edition %d", n + 1);
		}
		n++;
		p = (end != NULL) ? end : p + 1;
	}
	free(xml);
	return n;
}

uint32_t get_windows_build(const char* iso_path, const image_report_t* r, char* version, size_t version_size)
{
	uint8_t hdr[208];
	uint64_t xml_offset, xml_size;
	uint8_t* xml16 = NULL;
	char* xml = NULL;
	uint32_t build = 0;
	size_t i;

	if (version != NULL && version_size > 0)
		version[0] = 0;
	if (!HAS_WININST(r) || !read_iso_file_range(iso_path, r->wininst_path[0], 0, hdr, sizeof(hdr)))
		return 0;
	if (memcmp(hdr, "MSWIM\0\0\0", 8) != 0)
		return 0;
	/* WIM header: rhXmlData (reshdr_disk_short) at offset 0x48 */
	xml_size = (uint64_t)hdr[0x48] | ((uint64_t)hdr[0x49] << 8) | ((uint64_t)hdr[0x4a] << 16) |
		((uint64_t)hdr[0x4b] << 24) | ((uint64_t)hdr[0x4c] << 32) | ((uint64_t)hdr[0x4d] << 40) | ((uint64_t)hdr[0x4e] << 48);
	memcpy(&xml_offset, &hdr[0x50], 8);
	if (xml_size < 16 || xml_size > 4 * MB)
		return 0;
	xml16 = malloc((size_t)xml_size);
	xml = calloc(1, (size_t)xml_size / 2 + 1);
	if (xml16 == NULL || xml == NULL || !read_iso_file_range(iso_path, r->wininst_path[0], xml_offset, xml16, (size_t)xml_size))
		goto out;
	/* The metadata is UTF-16LE, and all we need is ASCII */
	for (i = 0; i < xml_size / 2; i++)
		xml[i] = (xml16[2 * i + 1] == 0) ? (char)xml16[2 * i] : '?';
	{
		char* b = strstr(xml, "<BUILD>");
		char* n = strstr(xml, "<DISPLAYNAME>");
		if (n == NULL)
			n = strstr(xml, "<NAME>");
		if (b != NULL)
			build = (uint32_t)strtoul(b + 7, NULL, 10);
		if (n != NULL && version != NULL) {
			char* s = strchr(n, '>') + 1;
			char* e = strchr(s, '<');
			if (e != NULL)
				snprintf(version, version_size, "%.*s", (int)(e - s), s);
		}
	}
	if (build != 0)
		uprintf("Windows build: %u%s%s", build, (version && version[0]) ? " - " : "", version ? version : "");
out:
	free(xml16);
	free(xml);
	return build;
}

/* ------------------------------------------------------------------------ */
/* Apply the customization to the target (ApplyWindowsCustomization)         */
/* ------------------------------------------------------------------------ */
bool apply_windows_customization(sink_t* sink, const char* iso_path, const image_report_t* r, const wue_options_t* w)
{
	char* xml;
	bool ok = true;

	if (w == NULL || w->flags == 0)
		return true;
	uprintf("Applying Windows customization:");
	xml = create_unattend_xml(r, w);
	if (xml == NULL)
		return false;

	if (w->flags & UNATTEND_SECUREBOOT_TPM_MINRAM) {
		/* In-place upgrade bypass: neuter appraiserres.dll, as Rufus does */
		static const char placeholder[] = "";
		if (sink_write_buffer(sink, "/sources/appraiserres.dll", placeholder, 0))
			uprintf("Created '/sources/appraiserres.dll' placeholder");
		/* Windows 11 24H2+ setup.exe wrapper */
		if (get_windows_build(iso_path, r, NULL, 0) >= 26000) {
			const char* arch = xml_arch(r);
			const char* res = (strcmp(arch, "arm64") == 0) ? "setup/setup_arm64.exe" : "setup/setup_x64.exe";
			void* orig = NULL, * wrapper;
			size_t orig_len = 0, wrapper_len = 0;
			wrapper = load_resource(res, &wrapper_len);
			if (wrapper != NULL && extract_iso_file(iso_path, "/sources/setup.exe", &orig, &orig_len)) {
				if (sink_write_buffer(sink, "/sources/setup.dll", orig, orig_len) &&
					sink_write_buffer(sink, "/sources/setup.exe", wrapper, wrapper_len))
					uprintf("Renamed '/sources/setup.exe' → '/sources/setup.dll' and created bypass wrapper");
			} else {
				uprintf("WARNING: Could not add the in-place upgrade wrapper");
			}
			free(orig);
			free(wrapper);
		}
	}

	if (w->flags & UNATTEND_WINPE_SETUP_MASK) {
		ok = sink_write_buffer(sink, "/autounattend.xml", xml, strlen(xml));
		if (ok)
			uprintf("Created '/autounattend.xml'");
	} else {
		ok = sink->mkdir(sink, "/sources/$OEM$/$$/Panther") &&
			sink_write_buffer(sink, "/sources/$OEM$/$$/Panther/unattend.xml", xml, strlen(xml));
		if (ok)
			uprintf("Created '/sources/$OEM$/$$/Panther/unattend.xml'");
	}
	free(xml);
	return ok;
}
