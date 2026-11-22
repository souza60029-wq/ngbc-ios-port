import os
import base64

print("=========================================")
print("   INICIANDO FÁBRICA AOT - NGBC iOS      ")
print("=========================================")

os.makedirs("src/converted_assets", exist_ok=True)

# Vamos gerar um PNG de 256x256 (um quadriculado vermelho e branco)
# Representação em base64 de uma imagem de teste válida
png_data = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAIAAADTED8xAAAABGdBTUEAALGPC/xhBQAAACBjSFJN"
    "AAB6JgAAgIQAAPoAAACA6AAAdTAAAOpgAAA6mAAAF3CculE8AAAABmJLR0QA/wD/AP+gvaeTAAAA"
    "VElEQVR42u3BAQ0AAADCoPdPbQ8HFAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA4G8wAAABJgH6VQAAAABJRU5ErkJggg=="
)

with open("src/converted_assets/texture.png", "wb") as f:
    f.write(png_data)

print("[1/1] Textura de teste (texture.png) gerada com sucesso na pasta converted_assets.")