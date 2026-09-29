# Właściciel SceneDynamicBuffer — potwierdzony 2026-09-27

## Twarde fakty

Nazwa bufora i jej **jedyny** referentujący `lea`:

```asm
0x009C116  lea r9, [rip + 0x8FFDEB]   ; 0x0099BF08 "Renderer::SceneDynamicBuffer"
0x009C11D  lea r8, [rbp + 0x110]
0x009C124  lea rdx, [rsp + 0x20]
0x009C129  call [rax + 0x120]          ; create
0x009C137  mov [rbx + 0xB90], rax      ; uchwyt
```

Sąsiad buforów, potwierdzony tą samą metodą:

```asm
0x009C16D  lea r9, [rip + ...]         ; 0x0099BF28 "Renderer::SceneUpdatableBuffer"
```

Sąsiedztwo stringów w obrazie zgadza się z kolejnością kreacji:

```
0x0099BEE8  Renderer::SceneImmutableBuffer
0x0099BF08  Renderer::SceneDynamicBuffer
0x0099BF28  Renderer::SceneUpdatableBuffer
0x0099BF48  Renderer::ExposureBuffer
```

## Właściciel: `rbx`, wprowadzony z `rcx`

```asm
0x009C030  mov [rsp + 8], rbx
0x009C035  push rbp
0x009C045  mov rbx, rcx          <- rbx = this
```

`rbx` trzymany jest w całej funkcji, a `rbx + 0x9C0` to obiekt urządzenia
(ładujemy z niego vtable: `mov rcx, [rbx+0x9C0]; mov rax, [rcx]; call [rax+0x120]`).

## Jedyny caller: `0x8368A`

```
0x0008367F  mov rcx, rdi
0x00083682  call 0x93800
0x00083687  mov rcx, rdi
0x0008368A  call 0x9C030          <- jedyne wywołanie w całym obrazie
0x0008368F  mov rcx, rdi
0x00083692  call 0x9C600
```

`0x8368A` jest w funkcji o entry `0x82CB0`, która na początku zeruje obiekt:

```asm
0x00082CBA  mov [rsp + 8], rcx
0x00082CCD  mov rdi, rcx
0x00082CD2  mov [rcx], rbp          ; 0
0x00082CD5  mov [rcx + 8], rbp      ; 0
...                             zeruje do +0x38
```

To konstruktor. **`0x9C030` to funkcja inicjalizująca bufory jakiegoś obiektu
graficznego — nie renderera z `beginRender`.**

## Dlatego `renderer + 0xB90` było zerowe

Mierzyłem z `VR_GetRenderer()`, czyli z `this` z `beginRender` (0x27A41789F70).
To jest `TRendererD3D12`. Bufory należą do innego obiektu, tworzonego przez
inny konstruktor. Odczyty `+0xB90`, `+0x9C0` zerowe są więc **poprawne** —
nie tam szukać.

## Zweryfikowany wzorzec kreacji (niezależny właściciel)

`0x80C3F` robi dokładnie to samo, dla buforów DOF:

```asm
0x00080C64  mov rcx, [rbx + 0x18]     ; urządzenie
0x00080C6B  lea r9, [rip + ...]       ; "Dof::DofPerObjectBuffer"
0x00080C7B  call [rax + 0x120]
0x00080C89  mov [rbx + 0x88], rax
0x00080C95  call 0x5C99A0            ; zwolnij poprzedni
```

Ten sam schemat, inny właściciel, inne sloty. To potwierdza, że `call [rax+0x120]`
to kreacja zasobu, a `0x5C99A0` to zwolnienie.

## Destruktor: `0x84600`

```asm
0x0008527F  mov rcx, [rbx + 0xB90]
0x00085286  test rcx, rcx
0x00085289  je  ...
0x0008528B  call 0x5C99A0
```

Idzie po `+0xB90`, `+0xB98`, `+0xBA0`, `+0xBA8`, `+0xB80`, `+0xB88` — cały run
uchwytów. To nie renderer, bo `beginRender` ma inne zachowanie.

## Co dalej

Trzeba znaleźć **obiekt wywołujący `0x82CB0`**, a nie `TRendererD3D12`.
Kandydat: klasa graficzna renderująca scenę (renderer wewnętrzny), inna niż
interfejs `TRendererD3D12` w vtable.

Następny krok: przechwycić `0x8368A` (call tworzący bufory) i odczytać
`rbx`/`rdi` w chwili wykonania — wtedy jest właściciel na pewno.
