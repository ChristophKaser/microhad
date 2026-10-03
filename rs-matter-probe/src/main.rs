// Target compiler/runtime probe. No network, production state, or radio access.
use core::convert::Infallible;
use core::mem::MaybeUninit;
use rs_matter::crypto::{
    default_crypto, Aead, CanonEcPoint, CanonPkcSharedSecret, CanonPkcSignature, Crypto,
    CryptoSensitive, CryptoSensitiveRef, Digest, HmacHash, Kdf, PbKdf, PublicKey, SecretKey,
    SigningSecretKey, TryCryptoRng, TryRng,
};
use rs_matter::dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET};
use rs_matter::error::Error;
use rs_matter::sc::pase::spake2p::{Spake2P, Spake2pVerifierData, SPAKE2P_ITERATION_COUNT};
use rs_matter::tlv::{TLVElement, TLVTag, TLVWrite};
use rs_matter::utils::init::InitMaybeUninit;
use rs_matter::utils::storage::WriteBuf;
use std::fs::File;
use std::io::Read;
use std::time::Instant;

// Actual OS entropy; read failures abort this test rather than returning weak bytes.
struct OsEntropy(File);
impl TryRng for OsEntropy {
    type Error = Infallible;
    fn try_next_u32(&mut self) -> Result<u32, Infallible> {
        let mut bytes = [0; 4];
        self.try_fill_bytes(&mut bytes)?;
        Ok(u32::from_ne_bytes(bytes))
    }
    fn try_next_u64(&mut self) -> Result<u64, Infallible> {
        let mut bytes = [0; 8];
        self.try_fill_bytes(&mut bytes)?;
        Ok(u64::from_ne_bytes(bytes))
    }
    fn try_fill_bytes(&mut self, bytes: &mut [u8]) -> Result<(), Infallible> {
        self.0.read_exact(bytes).expect("OS entropy read failed");
        Ok(())
    }
}
impl TryCryptoRng for OsEntropy {}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|value| format!("{value:02x}")).collect()
}

fn commissioner_size<C: Crypto>(_: &C) -> usize {
    core::mem::size_of::<rs_matter::onboard::Commissioner<'_, '_, &C>>()
}

fn setup_context<C: Crypto>(
    state: &mut Spake2P,
    crypto: &C,
    local: u16,
    peer: u16,
) -> Result<(), Error> {
    let context = state.start_context(crypto, local, peer, b"synthetic PBKDF request")?;
    state.finish_context::<C>(context, b"synthetic PBKDF response")
}

fn checks() -> Result<(), Error> {
    let entropy = OsEntropy(File::open("/dev/urandom").expect("OS entropy unavailable"));
    // Public test singleton scalar. Generated session/key-agreement keys use OS entropy.
    let mut singleton = [0; 32];
    singleton[31] = 1;
    let crypto = default_crypto(entropy, CryptoSensitiveRef::new(&singleton));

    let mut digest = crypto.hash()?;
    digest.update(b"abc")?;
    let mut hash = CryptoSensitive::<32>::new();
    digest.finish(&mut hash)?;
    assert_eq!(
        hex(hash.access()),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    println!("PASS SHA256 known answer");

    let mut derived = CryptoSensitive::<42>::new();
    crypto.kdf()?.expand(
        &[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12],
        CryptoSensitiveRef::new(&[0x0b; 22]),
        &[0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9],
        &mut derived,
    )?;
    assert_eq!(
        hex(derived.access()),
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"
    );
    let mut password_key = CryptoSensitive::<32>::new();
    crypto.pbkdf()?.derive(
        CryptoSensitiveRef::new(b"password"),
        1,
        b"salt",
        &mut password_key,
    )?;
    assert_eq!(
        hex(password_key.access()),
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"
    );
    println!("PASS HKDF and PBKDF2 known answers");

    let mut aead = crypto.aead()?;
    let key = CryptoSensitiveRef::new(&[0x11; 16]);
    let nonce = CryptoSensitiveRef::new(&[0x22; 13]);
    let mut encrypted = [0; 20];
    encrypted[..4].copy_from_slice(b"test");
    aead.encrypt_in_place(key, nonce, b"header", &mut encrypted, 4)?;
    let mut tampered = encrypted;
    tampered[0] ^= 1;
    assert!(aead
        .decrypt_in_place(key, nonce, b"header", &mut tampered)
        .is_err());
    assert_eq!(
        aead.decrypt_in_place(key, nonce, b"header", &mut encrypted)?,
        b"test"
    );
    println!("PASS AES CCM roundtrip and tamper rejection");

    let alice = crypto.generate_secret_key()?;
    let bob = crypto.generate_secret_key()?;
    let mut signature = CanonPkcSignature::new();
    alice.sign(b"rs-matter target probe", &mut signature)?;
    assert!(alice
        .pub_key()?
        .verify(b"rs-matter target probe", signature.reference())?);
    assert!(!alice
        .pub_key()?
        .verify(b"changed message", signature.reference())?);
    let mut shared_a = CanonPkcSharedSecret::new();
    let mut shared_b = CanonPkcSharedSecret::new();
    alice.derive_shared_secret(&bob.pub_key()?, &mut shared_a)?;
    bob.derive_shared_secret(&alice.pub_key()?, &mut shared_b)?;
    assert_eq!(shared_a.access(), shared_b.access());
    println!("PASS P256 ECDSA tamper rejection and ECDH agreement");

    let mut prover_storage = MaybeUninit::uninit();
    let prover = prover_storage.init_with(Spake2P::init());
    let mut verifier_storage = MaybeUninit::uninit();
    let verifier = verifier_storage.init_with(Spake2P::init());
    setup_context(prover, &crypto, 1, 2)?;
    setup_context(verifier, &crypto, 2, 1)?;
    let password = 20202021_u32.to_le_bytes();
    let salt = [0x55; 16];
    let mut verifier_data_storage = MaybeUninit::uninit();
    let verifier_data = verifier_data_storage.init_with(Spake2pVerifierData::init_with_pw(
        CryptoSensitiveRef::new(&password),
        &salt,
    ));
    let mut point_a = CanonEcPoint::new();
    let mut point_b = CanonEcPoint::new();
    let mut confirm_a = HmacHash::new();
    let mut confirm_b = HmacHash::new();
    let context = prover.setup_prover(
        &crypto,
        CryptoSensitiveRef::new(&password),
        &salt,
        SPAKE2P_ITERATION_COUNT,
        &mut point_a,
    )?;
    verifier.setup_verifier(
        &crypto,
        verifier_data,
        point_a.reference(),
        &mut point_b,
        &mut confirm_b,
    )?;
    prover
        .complete_prover(
            &crypto,
            &context,
            point_a.reference(),
            point_b.reference(),
            confirm_b.reference(),
            &mut confirm_a,
        )
        .expect("SPAKE2+ prover confirmation");
    let (_, _, verifier_key) = verifier
        .verify(confirm_a.reference())
        .expect("SPAKE2+ verifier confirmation");
    assert_eq!(prover.ke().access(), verifier_key.access());
    let mut bad_confirmation = *confirm_a.access();
    bad_confirmation[0] ^= 1;
    assert!(verifier
        .verify(CryptoSensitiveRef::new(&bad_confirmation))
        .is_err());
    println!("PASS SPAKE2 plus key agreement and invalid confirmation rejection");

    let mut bytes = [0; 32];
    let mut encoded = WriteBuf::new(&mut bytes);
    encoded.start_struct(&TLVTag::Anonymous)?;
    encoded.i16(&TLVTag::Context(1), 2315)?;
    encoded.u32(&TLVTag::Context(2), 800)?;
    encoded.end_container()?;
    let decoded = TLVElement::new(encoded.as_slice());
    assert_eq!(decoded.read::<i16>(1)?, 2315);
    assert_eq!(decoded.read::<u32>(2)?, 800);
    println!("PASS Matter TLV endian roundtrip");

    let mut matter_storage = Box::new(MaybeUninit::uninit());
    let _matter = matter_storage.init_with(rs_matter::Matter::init(
        &TEST_DEV_DET,
        TEST_DEV_COMM,
        &TEST_DEV_ATT,
        5540,
    ));
    println!("PASS Matter runtime initialization with public test credentials");
    println!(
        "SIZE matter_bytes={} commissioner_bytes={}",
        core::mem::size_of::<rs_matter::Matter>(),
        commissioner_size(&crypto)
    );
    Ok(())
}

fn main() {
    let started = Instant::now();
    checks().expect("rs-matter target checks failed");
    println!("PASS complete elapsed_ms={}", started.elapsed().as_millis());
}
