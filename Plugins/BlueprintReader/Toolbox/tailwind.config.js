/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        ue: {
          dark: '#1a1a1a',
          panel: '#252525',
          border: '#3a3a3a',
          accent: '#0080ff',
          'accent-hover': '#3399ff',
        },
      },
    },
  },
  plugins: [],
};
