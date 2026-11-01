-- Lists every media item of a Photos album as tab-separated fields:
-- id, filename, width, height, favorite, capture time (seconds since 1970-01-01 local time),
-- latitude, longitude (empty when unknown).
on run argv
	set albumName to item 1 of argv
	set outPath to item 2 of argv
	set epochDate to current date
	set year of epochDate to 1970
	set month of epochDate to January
	set day of epochDate to 1
	set time of epochDate to 0
	set outText to ""
	tell application "Photos"
		set picked to media items of album albumName
		repeat with m in picked
			set meta to properties of m
			set captureSeconds to ((get date of m) - epochDate)
			set latText to ""
			set lonText to ""
			try
				set loc to location of meta
				if (item 1 of loc) is not missing value then
					set latText to (item 1 of loc) as text
					set lonText to (item 2 of loc) as text
				end if
			end try
			set outText to outText & (id of meta) & tab & (filename of meta) & tab & (width of meta) & tab & (height of meta) & tab & (favorite of meta) & tab & captureSeconds & tab & latText & tab & lonText & linefeed
		end repeat
	end tell
	set outputFile to open for access POSIX file outPath with write permission
	set eof outputFile to 0
	write outText to outputFile as «class utf8»
	close access outputFile
	return count of picked
end run
