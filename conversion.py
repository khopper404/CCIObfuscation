import base64, re
from IPython.display import Image, display
#input output text here from MASKED BEGIN to MASKED END inclusive
text = """
"""

m = re.search(r"MASKED_BEGIN\s*(.*?)\s*MASKED_END", text, re.S)
if m:
    text = m.group(1)

b64 = re.sub(r"[^A-Za-z0-9+/]", "", text.split("=")[0])
b64 += "=" * (-len(b64) % 4)

data = base64.b64decode(b64)
print(len(data), "bytes, starts", data[:2].hex(), "ends", data[-2:].hex())
open("frame.jpg", "wb").write(data)
display(Image("frame.jpg"))
