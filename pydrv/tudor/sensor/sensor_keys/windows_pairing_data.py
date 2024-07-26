# user account security identifier from a registry key
WINBIO_SAMPLE_SID = b"\x03\x00\x00\x00\x1c\x00\x00\x00\x01\x05\x00\x00\x00\x00\x00\x05\x15\x00\x00\x00\x0c\xbb\x01\x4e\x6d\xdd\x74\xeb\x5b\x41\xeb\x98\xe9\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"

# private key exported by palCryptoEccExportPrivateKey
PRIV_KEY = b"\x6a\x7f\xb2\xf8\x0d\xdf\x0c\xdf\x18\xfe\x1d\x14\x4a\x80\x9f\x58\xe4\x14\x8a\x80\xcb\x9a\x75\xda\x82\x19\x55\x06\xce\x27\x1a\xf7"

# received host certificate from cmd pair
RECV_HOST_CERT = b"\x3f\x5f\x17\x00\x3d\xf7\xe9\x67\xc0\xd8\x52\x6a\xea\x3e\x08\x0b\x10\x32\xc1\x7d\x90\xd3\x9e\x50\x44\x44\x49\x20\xbb\xad\x14\xe0\xc2\xdb\xf9\xa8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x42\x8d\x94\x68\x1d\x09\xa6\x7a\xa1\xb6\xa1\x86\x3d\x25\x55\xc8\x7e\xa2\xfe\x18\x18\x38\xfd\x28\xc8\xc9\xa6\xd5\xb0\x21\xc6\xee\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x20\x00\x4d\x63\x1a\x68\xa5\x8d\x4c\xff\x6f\xd2\xef\x97\x78\x09\xe8\x2f\x0f\x1d\x61\xb1\xe2\xe9\xf7\xba\x47\x7b\x6f\xdf\xb7\xd4\x05\x6c\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"

# received sensor certificate from cmd pair
SENSOR_CERT = b"\x3f\x5f\x17\x00\x32\x29\x44\x49\x1e\x0e\x65\x4d\x1f\x49\xe7\x23\xa2\x33\x25\x0f\x09\x9d\xdb\x99\x47\xdb\xb2\x99\x27\x4f\xe6\xb1\x6d\x6c\x88\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x4e\xbf\x09\x77\x46\x48\x52\x1e\xee\x9b\x75\x45\x0b\x7d\x86\xb3\x2e\xa9\x8c\x11\xfc\xf3\xf4\xd5\x65\xa2\x3c\x30\x4b\x18\xbd\x86\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x47\x00\x30\x45\x02\x20\x04\x01\x61\x18\xe2\x32\xd4\xc1\xb3\x69\xd3\x20\x48\x08\x36\x19\xfd\x7c\x66\x5b\x37\x2e\x13\xb3\xcf\x24\xb7\xe1\xc8\xbb\x12\x29\x02\x21\x00\xdc\x4b\x3b\xdd\xff\x2b\x50\x4e\x85\xed\xba\x2d\x22\xb5\xe8\xb2\x1b\x7a\x89\x05\xdb\x1a\x0c\x52\xf4\xd0\xe4\x5b\xd4\x06\x25\xca\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
