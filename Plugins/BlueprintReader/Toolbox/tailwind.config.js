/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        ue: {
          dark: '#1a1a1a',
          panel: '#252525',
          panel2: '#2d2d2d',
          border: '#3a3a3a',
          accent: '#E87722',
          'accent-hover': '#F59340',
          title: '#1c1c1c',
        },
      },
    },
  },
  plugins: [],
};
